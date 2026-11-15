#pragma once

#include <webgpu/webgpu_cpp.h>

#include <cstdint>
#include <vector>

class Scene;
class Camera;

struct WireVertex {
    float position[2];
    float colour[3];
};

struct RasterVertex {
    float position[3];
    float colour[3];
};

class WebGPURenderer {

public:
    WebGPURenderer(int width, int height);

    bool isReady() const;

    void drawWireframe(
        const Scene& scene,
        const Camera& camera
    );

    void drawRasterised(
        const Scene& scene,
        const Camera& camera
    );

    void drawRaytraced(
        const Scene& scene,
        const Camera& camera
    );

private:

    int width;
    int height;

    bool ready = false;
    bool adapterReady = false;
    bool deviceReady = false;

    // WebGPU resources.
    // The instance is declared first so it outlives the other handles.
    wgpu::Instance instance;
    wgpu::Adapter adapter;
    wgpu::Device device;
    wgpu::Surface surface;

    wgpu::ShaderModule shaderModule;
    wgpu::RenderPipeline pipeline;
    wgpu::Buffer vertexBuffer;
    wgpu::Texture depthTexture;
    wgpu::TextureView depthTextureView;

    wgpu::TextureFormat surfaceFormat =
        wgpu::TextureFormat::Undefined;

    uint64_t vertexBufferCapacity = 0;

    // Ray-tracing resources.
    wgpu::ShaderModule rayShaderModule;
    wgpu::RenderPipeline rayPipeline;

    wgpu::BindGroupLayout rayBindGroupLayout;
    wgpu::PipelineLayout rayPipelineLayout;
    wgpu::BindGroup rayBindGroup;

    wgpu::Buffer rayTriangleBuffer;
    wgpu::Buffer rayLightBuffer;
    wgpu::Buffer rayCameraBuffer;

    uint64_t rayTriangleBufferCapacity = 0;
    uint64_t rayLightBufferCapacity = 0;

    bool initialise();

    bool createDevice();

    bool configureSurface();

    bool createWireframePipeline();
    bool createRasterisedPipeline();
    bool createRaytracedPipeline();

    void ensureVertexBuffer(uint64_t requiredBytes);

    void render(const std::vector<WireVertex>& vertices);
    void render(const std::vector<RasterVertex>& vertices);
    void renderRaytraced();

    void ensureRayBuffers(
        uint64_t triangleBytes,
        uint64_t lightBytes
    );

    void updateRayBindGroup();
};
