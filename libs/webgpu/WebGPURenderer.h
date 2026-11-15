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

class WebGPURenderer {

public:
    WebGPURenderer(int width, int height);

    bool isReady() const;

    void drawWireframe(
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

    wgpu::TextureFormat surfaceFormat =
        wgpu::TextureFormat::Undefined;

    uint64_t vertexBufferCapacity = 0;

    bool initialise();

    bool createDevice();

    bool configureSurface();

    bool createPipeline();

    void ensureVertexBuffer(uint64_t requiredBytes);

    void render(const std::vector<WireVertex>& vertices);
};
