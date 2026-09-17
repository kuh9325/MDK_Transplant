// Metal presentation backend (Objective-C++ — confined to the renderer/
// platform boundary; core code sees no Metal headers).
//
// Path: IndexedFramebuffer (600x360 8bpp) --CPU palette expansion--> BGRA8
// staging --> 600x360 MTLTexture --> textured quad --> CAMetalLayer
// drawable inside the SDL window (SDL_Metal_CreateView).
//
// NATIVE PORT PROJECT DECISIONS:
//   - CPU palette expansion is the simple, reliable Phase 3A choice; the
//     indexed framebuffer stays the authoritative working surface.
//   - layer.displaySyncEnabled = YES paces nextDrawable to vsync, so the
//     loop never busy-spins.
//   - Nearest-neighbor sampling preserves the indexed-era pixel look.

#include "renderer/presenter.h"

#include "core/compat.h"
#include "core/framebuffer.h"
#include "core/log.h"
#include "core/viewport.h"

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <SDL3/SDL_metal.h>
#import <SDL3/SDL_video.h>

#include <cstdlib>

namespace {

constexpr const char* kTag = "renderer";

const char* kShaderSource = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct VSOut {
  float4 position [[position]];
  float2 uv;
};

vertex VSOut presentVS(uint vid [[vertex_id]]) {
  // Metal NDC y=+1 is the screen top; texture v=0 is texel row 0
  // (framebuffer top). Pair them so the image is not upside down.
  constexpr float2 quad[4] = {{-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
  constexpr float2 uvs[4] = {{0, 1}, {1, 1}, {0, 0}, {1, 0}};
  VSOut o;
  o.position = float4(quad[vid], 0, 1);
  o.uv = uvs[vid];
  return o;
}

fragment float4 presentFS(VSOut in [[stage_in]],
                          texture2d<float> tex [[texture(0)]],
                          sampler samp [[sampler(0)]]) {
  return tex.sample(samp, in.uv);
}
)MSL";

class MetalPresenter final : public mdk::Presenter {
public:
  MetalPresenter() = default;
  ~MetalPresenter() override { shutdown(); }

  bool init(SDL_Window* window, std::string* error) {
    @autoreleasepool {
      metalView_ = SDL_Metal_CreateView(window);
      if (!metalView_) {
        setError(error, "SDL_Metal_CreateView failed: %s", SDL_GetError());
        return false;
      }
      layer_ = (__bridge CAMetalLayer*)SDL_Metal_GetLayer(metalView_);
      device_ = MTLCreateSystemDefaultDevice();
      if (!device_) {
        setError(error, "MTLCreateSystemDefaultDevice returned nil");
        return false;
      }
      layer_.device = device_;
      layer_.pixelFormat = MTLPixelFormatBGRA8Unorm;
      layer_.displaySyncEnabled = YES; // vsync pacing — no busy-spin
      layer_.framebufferOnly = YES;

      queue_ = [device_ newCommandQueue];
      staging_.resize(mdk::compat::kWorkWidth * mdk::compat::kWorkHeight * 4);

      MTLTextureDescriptor* td = [MTLTextureDescriptor
          texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                       width:mdk::compat::kWorkWidth
                                      height:mdk::compat::kWorkHeight
                                   mipmapped:NO];
      td.usage = MTLTextureUsageShaderRead;
      td.storageMode = MTLStorageModeShared;
      texture_ = [device_ newTextureWithDescriptor:td];

      NSError* err = nil;
      id<MTLLibrary> lib =
          [device_ newLibraryWithSource:@(kShaderSource)
                                options:nil
                                  error:&err];
      if (!lib) {
        setError(error, "MSL compile failed: %s",
                 [[err localizedDescription] UTF8String]);
        return false;
      }
      MTLRenderPipelineDescriptor* pd =
          [[MTLRenderPipelineDescriptor alloc] init];
      pd.vertexFunction = [lib newFunctionWithName:@"presentVS"];
      pd.fragmentFunction = [lib newFunctionWithName:@"presentFS"];
      pd.colorAttachments[0].pixelFormat = layer_.pixelFormat;
      pipeline_ = [device_ newRenderPipelineStateWithDescriptor:pd
                                                        error:&err];
      if (!pipeline_) {
        setError(error, "pipeline creation failed: %s",
                 [[err localizedDescription] UTF8String]);
        return false;
      }

      MTLSamplerDescriptor* sd = [[MTLSamplerDescriptor alloc] init];
      sd.minFilter = MTLSamplerMinMagFilterNearest;
      sd.magFilter = MTLSamplerMinMagFilterNearest;
      sampler_ = [device_ newSamplerStateWithDescriptor:sd];

      int w = 0, h = 0;
      SDL_GetWindowSizeInPixels(window, &w, &h);
      drawableSizeChanged(w, h);

      // Debug verification: MDK_VERIFY_RENDER=/path/out.ppm renders one
      // frame into a 640x480 offscreen canvas and dumps the GPU result —
      // proves upload + shader + viewport + orientation end-to-end.
      if (const char* p = std::getenv("MDK_VERIFY_RENDER")) {
        verifyPath_ = p;
        MTLTextureDescriptor* vd = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                         width:mdk::compat::kPresentWidth
                                        height:mdk::compat::kPresentHeight
                                     mipmapped:NO];
        vd.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
        vd.storageMode = MTLStorageModeShared;
        verifyTex_ = [device_ newTextureWithDescriptor:vd];
      }

      mdk::log::info(kTag, "Metal device: %s",
                     [[device_ name] UTF8String]);
      mdk::log::info(kTag, "drawable %dx%d, framebuffer %dx%d indexed",
                     drawableW_, drawableH_, mdk::compat::kWorkWidth,
                     mdk::compat::kWorkHeight);
      return true;
    }
  }

  void drawableSizeChanged(int w, int h) override {
    if (w <= 0 || h <= 0) {
      return;
    }
    drawableW_ = w;
    drawableH_ = h;
    @autoreleasepool {
      layer_.drawableSize = CGSizeMake(w, h);
    }
  }

  bool present(const mdk::IndexedFramebuffer& fb,
               const mdk::Palette& palette) override {
    @autoreleasepool {
      mdk::expandToBGRA(fb, palette, staging_.data());
      [texture_ replaceRegion:MTLRegionMake2D(0, 0, fb.width(), fb.height())
                  mipmapLevel:0
                    withBytes:staging_.data()
                  bytesPerRow:fb.width() * 4];

      id<CAMetalDrawable> drawable = [layer_ nextDrawable];
      if (!drawable) {
        return false;
      }
      MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
      rp.colorAttachments[0].texture = drawable.texture;
      rp.colorAttachments[0].loadAction = MTLLoadActionClear;
      rp.colorAttachments[0].clearColor =
          MTLClearColorMake(0, 0, 0, 1);
      rp.colorAttachments[0].storeAction = MTLStoreActionStore;

      id<MTLCommandBuffer> cmd = [queue_ commandBuffer];
      id<MTLRenderCommandEncoder> enc =
          [cmd renderCommandEncoderWithDescriptor:rp];

      const mdk::RectD r =
          mdk::presentationRect(drawableW_, drawableH_);
      MTLViewport vp{r.x, r.y, r.w, r.h, 0.0, 1.0};
      [enc setViewport:vp];
      [enc setRenderPipelineState:pipeline_];
      [enc setFragmentTexture:texture_ atIndex:0];
      [enc setFragmentSamplerState:sampler_ atIndex:0];
      [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip
              vertexStart:0
              vertexCount:4];
      [enc endEncoding];
      [cmd presentDrawable:drawable];

      if (verifyTex_) {
        // Second identical pass into the virtual 640x480 canvas.
        MTLRenderPassDescriptor* vrp =
            [MTLRenderPassDescriptor renderPassDescriptor];
        vrp.colorAttachments[0].texture = verifyTex_;
        vrp.colorAttachments[0].loadAction = MTLLoadActionClear;
        vrp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
        vrp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> venc =
            [cmd renderCommandEncoderWithDescriptor:vrp];
        const mdk::RectD vr = mdk::presentationRect(
            mdk::compat::kPresentWidth, mdk::compat::kPresentHeight);
        MTLViewport vvp{vr.x, vr.y, vr.w, vr.h, 0.0, 1.0};
        [venc setViewport:vvp];
        [venc setRenderPipelineState:pipeline_];
        [venc setFragmentTexture:texture_ atIndex:0];
        [venc setFragmentSamplerState:sampler_ atIndex:0];
        [venc drawPrimitives:MTLPrimitiveTypeTriangleStrip
                 vertexStart:0
                 vertexCount:4];
        [venc endEncoding];
      }

      [cmd commit];
      if (verifyTex_) {
        [cmd waitUntilCompleted];
        dumpVerifyTexture();
        verifyTex_ = nil; // once
      }
      return true;
    }
  }

  const char* name() const override { return "metal"; }

  void shutdown() {
    if (metalView_) {
      SDL_Metal_DestroyView(metalView_);
      metalView_ = nullptr;
      layer_ = nil;
    }
  }

private:
  void dumpVerifyTexture() {
    const int w = mdk::compat::kPresentWidth, h = mdk::compat::kPresentHeight;
    std::vector<std::uint8_t> bgra(static_cast<std::size_t>(w) * h * 4);
    [verifyTex_ getBytes:bgra.data()
             bytesPerRow:static_cast<NSUInteger>(w) * 4
              fromRegion:MTLRegionMake2D(0, 0, w, h)
             mipmapLevel:0];
    if (FILE* f = std::fopen(verifyPath_.c_str(), "wb")) {
      std::fprintf(f, "P6\n%d %d\n255\n", w, h);
      for (std::size_t i = 0; i < bgra.size(); i += 4) {
        std::fputc(bgra[i + 2], f);
        std::fputc(bgra[i + 1], f);
        std::fputc(bgra[i + 0], f);
      }
      std::fclose(f);
      mdk::log::info(kTag, "verify render dumped to %s",
                     verifyPath_.c_str());
    }
  }

  static void setError(std::string* error, const char* fmt, ...) {
    if (!error) {
      return;
    }
    char buf[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    *error = buf;
  }

  SDL_MetalView metalView_ = nullptr;
  CAMetalLayer* layer_ = nil;
  id<MTLDevice> device_ = nil;
  id<MTLCommandQueue> queue_ = nil;
  id<MTLTexture> texture_ = nil;
  id<MTLRenderPipelineState> pipeline_ = nil;
  id<MTLSamplerState> sampler_ = nil;
  id<MTLTexture> verifyTex_ = nil;
  std::string verifyPath_;
  std::vector<std::uint8_t> staging_;
  int drawableW_ = 0, drawableH_ = 0;
};

} // namespace

namespace mdk {

std::unique_ptr<Presenter> createMetalPresenter(void* sdlWindow,
                                                std::string* error) {
  auto p = std::make_unique<MetalPresenter>();
  if (!p->init(static_cast<SDL_Window*>(sdlWindow), error)) {
    return nullptr;
  }
  return p;
}

} // namespace mdk
