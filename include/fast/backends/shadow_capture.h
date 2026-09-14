#pragma once

#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>
#include <ostream>
#include <stdexcept>

// SDS1: little-endian u32 magic, width, height, slice count; then each row is
// u32 run count followed by (u16 depth, u32 length) pairs. No lossy conversion.
//
// firstSlice exists because the two caster layers can share one array: the actor slices sit right after the
// world ones in that arrangement, so writing them means starting partway in rather than at zero.
inline void WriteShadowDepthCapture(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* texture,
                                    UINT slices, std::ostream& out, UINT firstSlice = 0) {
    if (!device || !context || !texture)
        throw std::runtime_error("No shadow texture");
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    if ((desc.Format != DXGI_FORMAT_R16_TYPELESS && desc.Format != DXGI_FORMAT_D16_UNORM &&
         desc.Format != DXGI_FORMAT_R16_UNORM) ||
        desc.SampleDesc.Count != 1 || slices == 0 || firstSlice >= desc.ArraySize ||
        slices > desc.ArraySize - firstSlice || desc.Width > 8192 || desc.Height > 8192)
        throw std::runtime_error("Unsupported shadow capture dimensions or format");
    const UINT sourceMips = desc.MipLevels;
    desc.MipLevels = desc.ArraySize = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = desc.MiscFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging)))
        throw std::runtime_error("Cannot allocate shadow readback");
    uint64_t bytes = 0;
    const auto write = [&](auto value) {
        bytes += sizeof(value);
        if (bytes > 512ull * 1024 * 1024)
            throw std::runtime_error("Capture exceeds 512 MiB limit");
        out.write(reinterpret_cast<const char*>(&value), sizeof(value));
        if (!out)
            throw std::runtime_error("Cannot write shadow capture");
    };
    write(uint32_t(0x31534453));
    write(uint32_t(desc.Width));
    write(uint32_t(desc.Height));
    write(uint32_t(slices));
    for (UINT slice = 0; slice < slices; ++slice) {
        context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, texture,
                                       D3D11CalcSubresource(0, firstSlice + slice, sourceMips), nullptr);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
            throw std::runtime_error("Cannot read shadow depth");
        try {
            for (UINT y = 0; y < desc.Height; ++y) {
                const auto* input =
                    reinterpret_cast<const uint16_t*>(static_cast<const char*>(mapped.pData) + y * mapped.RowPitch);
                uint32_t runs = 1;
                for (UINT x = 1; x < desc.Width; ++x)
                    runs += input[x] != input[x - 1];
                write(runs);
                for (UINT x = 0; x < desc.Width;) {
                    UINT end = x + 1;
                    while (end < desc.Width && input[end] == input[x])
                        ++end;
                    write(input[x]);
                    write(uint32_t(end - x));
                    x = end;
                }
            }
        } catch (...) {
            context->Unmap(staging.Get(), 0);
            throw;
        }
        context->Unmap(staging.Get(), 0);
    }
}

// SDZ1: the SCENE's depth buffer, which is a different thing from the shadow map above and needs its own
// format for one reason -- it is 24-bit, not 16. Same little-endian header (magic, width, height, slice
// count) and same per-row run encoding, but the depth of a run is u32, holding the D24_UNORM value in its
// low 24 bits. Truncating it to 16 to reuse SDS1 would throw away the precision that decides whether a
// reconstructed receiver lands in front of or behind the surface it came from, which is the whole question
// a capture is opened to answer.
//
// This is the receiver the shadow map cannot supply. A shadow map stores the surface NEAREST THE LIGHT, so
// unprojecting it gives back the lit surface and nothing else: the wall standing in the castle's shadow is
// behind the castle along the light and was never written to it. The scene depth is taken from the camera
// instead, so every visible surface is in it -- shadowed ones included -- and the reproduction can finally
// show the shadow where it actually falls.
inline void WriteSceneDepthCapture(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* texture,
                                   std::ostream& out) {
    if (!device || !context || !texture)
        throw std::runtime_error("No scene depth texture");
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    if (desc.Format != DXGI_FORMAT_R24G8_TYPELESS && desc.Format != DXGI_FORMAT_D24_UNORM_S8_UINT &&
        desc.Format != DXGI_FORMAT_R24_UNORM_X8_TYPELESS)
        throw std::runtime_error("Scene depth is not a 24-bit depth format");
    // Multisampled depth cannot be resolved (no ResolveSubresource for depth formats) and cannot be mapped,
    // so there is no honest way to read it here. Reported by name rather than silently producing a capture
    // with no receiver in it.
    if (desc.SampleDesc.Count != 1)
        throw std::runtime_error("Scene depth is multisampled; turn MSAA off to capture the receiver");
    if (desc.Width == 0 || desc.Height == 0 || desc.Width > 8192 || desc.Height > 8192)
        throw std::runtime_error("Unsupported scene depth dimensions");

    desc.MipLevels = desc.ArraySize = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = desc.MiscFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging)))
        throw std::runtime_error("Cannot allocate scene depth readback");

    uint64_t bytes = 0;
    const auto write = [&](auto value) {
        bytes += sizeof(value);
        if (bytes > 512ull * 1024 * 1024)
            throw std::runtime_error("Capture exceeds 512 MiB limit");
        out.write(reinterpret_cast<const char*>(&value), sizeof(value));
        if (!out)
            throw std::runtime_error("Cannot write scene depth capture");
    };
    write(uint32_t(0x315A4453));
    write(uint32_t(desc.Width));
    write(uint32_t(desc.Height));
    write(uint32_t(1));

    context->CopyResource(staging.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
        throw std::runtime_error("Cannot read scene depth");
    try {
        for (UINT y = 0; y < desc.Height; ++y) {
            const auto* row =
                reinterpret_cast<const uint32_t*>(static_cast<const char*>(mapped.pData) + y * mapped.RowPitch);
            // The stencil plane shares the word and is not part of the depth. Masked off here rather than
            // in the reader, so the file holds depth and only depth.
            const auto depthAt = [row](UINT x) { return row[x] & 0x00FFFFFFu; };
            uint32_t runs = 1;
            for (UINT x = 1; x < desc.Width; ++x)
                runs += depthAt(x) != depthAt(x - 1);
            write(runs);
            for (UINT x = 0; x < desc.Width;) {
                UINT end = x + 1;
                while (end < desc.Width && depthAt(end) == depthAt(x))
                    ++end;
                write(uint32_t(depthAt(x)));
                write(uint32_t(end - x));
                x = end;
            }
        }
    } catch (...) {
        context->Unmap(staging.Get(), 0);
        throw;
    }
    context->Unmap(staging.Get(), 0);
}
