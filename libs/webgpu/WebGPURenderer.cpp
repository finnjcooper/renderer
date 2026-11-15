#include "WebGPURenderer.h"

#include "Scene.h"
#include "Camera.h"
#include "Model.h"
#include "ModelTriangle.h"

#include <iostream>
#include <vector>
#include <limits>
#include <utility>
#include <cstddef>
#include <cstdint>


// ------------------------------------------------------------
// WGSL shaders
// ------------------------------------------------------------

static const char* wireframeShader = R"(
    struct VertexOutput {
        @builtin(position) position: vec4<f32>,
        @location(0) colour: vec3<f32>,
    };

    @vertex
    fn vertexMain(
        @location(0) position: vec2<f32>,
        @location(1) colour: vec3<f32>
    ) -> VertexOutput {

        var output: VertexOutput;

        output.position = vec4<f32>(
            position.x,
            position.y,
            0.5,
            1.0
        );

        output.colour = colour;

        return output;
    }

    @fragment
    fn fragmentMain(
        input: VertexOutput
    ) -> @location(0) vec4<f32> {

        return vec4<f32>(
            input.colour,
            1.0
        );
    }
)";


// ------------------------------------------------------------
// Constructor
// ------------------------------------------------------------

WebGPURenderer::WebGPURenderer(int width, int height)
    : width(width), height(height)
{
    ready = initialise();

    if (!ready) {
        std::cerr << "WebGPU initialisation failed." << std::endl;
    }
}


// ------------------------------------------------------------
// Public functions
// ------------------------------------------------------------

bool WebGPURenderer::isReady() const {
    return ready;
}


void WebGPURenderer::drawWireframe(
    const Scene& scene,
    const Camera& camera
) {
    if (!ready) return;

    std::vector<WireVertex> vertices;

    const float canvasScale =
        static_cast<float>(height) / 2.0f;

    // --------------------------------------------------------
    // Convert every model triangle into three GPU line segments.
    //
    // Triangle:
    //
    //       A
    //      / \
    //     /   \
    //    B-----C
    //
    // Line list:
    // A-B, B-C, C-A
    // --------------------------------------------------------

    for (const auto& model : scene.models) {

        const glm::vec3 colour = model.material.diffuse;

        for (const auto& triangle : model.triangles) {

            // Use your existing camera projection.
            const CanvasPoint a =
                camera.projectVertex(triangle[0], canvasScale);

            const CanvasPoint b =
                camera.projectVertex(triangle[1], canvasScale);

            const CanvasPoint c =
                camera.projectVertex(triangle[2], canvasScale);

            // Ignore triangles behind the camera for now.
            if (a.depth <= 0.0f ||
                b.depth <= 0.0f ||
                c.depth <= 0.0f) {
                continue;
            }

            // Convert screen coordinates into WebGPU
            // normalised device coordinates.
            //
            // X: [0, width]  -> [-1, 1]
            // Y: [0, height] -> [1, -1]

            auto makeVertex = [&](const CanvasPoint& point) {

                WireVertex vertex{};

                vertex.position[0] =
                    (2.0f * point.x / width) - 1.0f;

                vertex.position[1] =
                    1.0f - (2.0f * point.y / height);

                vertex.colour[0] = colour.r;
                vertex.colour[1] = colour.g;
                vertex.colour[2] = colour.b;

                return vertex;
            };

            const WireVertex va = makeVertex(a);
            const WireVertex vb = makeVertex(b);
            const WireVertex vc = makeVertex(c);

            // A -> B
            vertices.push_back(va);
            vertices.push_back(vb);

            // B -> C
            vertices.push_back(vb);
            vertices.push_back(vc);

            // C -> A
            vertices.push_back(vc);
            vertices.push_back(va);
        }
    }

    render(vertices);
}


// ------------------------------------------------------------
// Initialise WebGPU
// ------------------------------------------------------------

bool WebGPURenderer::initialise() {

    // TimedWaitAny is required for the WaitAny calls below.
    wgpu::InstanceFeatureName timedWaitAny =
        wgpu::InstanceFeatureName::TimedWaitAny;

    wgpu::InstanceDescriptor instanceDescriptor{};

    instanceDescriptor.requiredFeatureCount = 1;
    instanceDescriptor.requiredFeatures = &timedWaitAny;

    instance = wgpu::CreateInstance(&instanceDescriptor);

    if (!createDevice()) {
        return false;
    }

    if (!configureSurface()) {
        return false;
    }

    if (!createPipeline()) {
        return false;
    }

    return true;
}


// ------------------------------------------------------------
// Create WebGPU adapter and device
// ------------------------------------------------------------

bool WebGPURenderer::createDevice() {

    // Create a surface associated with our HTML canvas.
    wgpu::EmscriptenSurfaceSourceCanvasHTMLSelector canvasDescriptor{};

    canvasDescriptor.selector = "#webgpu-canvas";

    wgpu::SurfaceDescriptor surfaceDescriptor{};

    surfaceDescriptor.nextInChain = &canvasDescriptor;

    surface = instance.CreateSurface(&surfaceDescriptor);


    // Request an adapter compatible with our surface.
    wgpu::RequestAdapterOptions adapterOptions{};

    adapterOptions.compatibleSurface = surface;

    adapterReady = false;

    wgpu::Future adapterFuture = instance.RequestAdapter(
        &adapterOptions,
        wgpu::CallbackMode::WaitAnyOnly,

        [this](
            wgpu::RequestAdapterStatus status,
            wgpu::Adapter requestedAdapter,
            wgpu::StringView
        ) {

            if (status == wgpu::RequestAdapterStatus::Success) {
                adapter = std::move(requestedAdapter);
                adapterReady = true;
            }
        }
    );

    // Wait for the asynchronous adapter request.
    instance.WaitAny(
        adapterFuture,
        std::numeric_limits<uint64_t>::max()
    );

    if (!adapterReady) {
        std::cerr << "Could not acquire WebGPU adapter."
                  << std::endl;

        return false;
    }


    // Request a device from the adapter.
    wgpu::DeviceDescriptor deviceDescriptor{};

    deviceReady = false;

    wgpu::Future deviceFuture = adapter.RequestDevice(
        &deviceDescriptor,
        wgpu::CallbackMode::WaitAnyOnly,

        [this](
            wgpu::RequestDeviceStatus status,
            wgpu::Device requestedDevice,
            wgpu::StringView
        ) {

            if (status == wgpu::RequestDeviceStatus::Success) {
                device = std::move(requestedDevice);
                deviceReady = true;
            }
        }
    );

    instance.WaitAny(
        deviceFuture,
        std::numeric_limits<uint64_t>::max()
    );

    if (!deviceReady) {
        std::cerr << "Could not acquire WebGPU device."
                  << std::endl;

        return false;
    }

    std::cout << "WebGPU device initialised." << std::endl;

    return true;
}


// ------------------------------------------------------------
// Configure the HTML canvas surface
// ------------------------------------------------------------

bool WebGPURenderer::configureSurface() {

    wgpu::SurfaceCapabilities capabilities{};

    surface.GetCapabilities(adapter, &capabilities);

    if (capabilities.formatCount == 0) {
        std::cerr << "No supported WebGPU surface formats."
                  << std::endl;

        return false;
    }

    // Use the first supported surface format.
    surfaceFormat = capabilities.formats[0];

    wgpu::SurfaceConfiguration config{};

    config.device = device;
    config.format = surfaceFormat;

    config.usage = wgpu::TextureUsage::RenderAttachment;

    config.width = static_cast<uint32_t>(width);
    config.height = static_cast<uint32_t>(height);

    config.presentMode = wgpu::PresentMode::Fifo;
    config.alphaMode = wgpu::CompositeAlphaMode::Auto;

    surface.Configure(&config);

    return true;
}


// ------------------------------------------------------------
// Create the line-list render pipeline
// ------------------------------------------------------------

bool WebGPURenderer::createPipeline() {

    // --------------------------------------------------------
    // Shader module
    // --------------------------------------------------------

    wgpu::ShaderSourceWGSL shaderSource{};

    shaderSource.code = wireframeShader;

    wgpu::ShaderModuleDescriptor shaderDescriptor{};

    shaderDescriptor.nextInChain = &shaderSource;

    shaderModule =
        device.CreateShaderModule(&shaderDescriptor);


    // --------------------------------------------------------
    // Vertex attributes
    //
    // WireVertex:
    //
    // float position[2];
    // float colour[3];
    //
    // Position = shader location 0
    // Colour   = shader location 1
    // --------------------------------------------------------

    wgpu::VertexAttribute attributes[2]{};

    attributes[0].format = wgpu::VertexFormat::Float32x2;
    attributes[0].offset = offsetof(WireVertex, position);
    attributes[0].shaderLocation = 0;

    attributes[1].format = wgpu::VertexFormat::Float32x3;
    attributes[1].offset = offsetof(WireVertex, colour);
    attributes[1].shaderLocation = 1;


    wgpu::VertexBufferLayout vertexBufferLayout{};

    vertexBufferLayout.arrayStride = sizeof(WireVertex);
    vertexBufferLayout.stepMode = wgpu::VertexStepMode::Vertex;
    vertexBufferLayout.attributeCount = 2;
    vertexBufferLayout.attributes = attributes;


    // --------------------------------------------------------
    // Vertex shader state
    // --------------------------------------------------------

    wgpu::VertexState vertexState{};

    vertexState.module = shaderModule;
    vertexState.entryPoint = "vertexMain";
    vertexState.bufferCount = 1;
    vertexState.buffers = &vertexBufferLayout;


    // --------------------------------------------------------
    // Fragment shader state
    // --------------------------------------------------------

    wgpu::ColorTargetState colourTarget{};

    colourTarget.format = surfaceFormat;
    colourTarget.writeMask = wgpu::ColorWriteMask::All;

    wgpu::FragmentState fragmentState{};

    fragmentState.module = shaderModule;
    fragmentState.entryPoint = "fragmentMain";
    fragmentState.targetCount = 1;
    fragmentState.targets = &colourTarget;


    // --------------------------------------------------------
    // Primitive state
    //
    // LineList means every pair of vertices is a line.
    // --------------------------------------------------------

    wgpu::PrimitiveState primitiveState{};

    primitiveState.topology = wgpu::PrimitiveTopology::LineList;
    primitiveState.stripIndexFormat = wgpu::IndexFormat::Undefined;
    primitiveState.frontFace = wgpu::FrontFace::CCW;
    primitiveState.cullMode = wgpu::CullMode::None;


    // --------------------------------------------------------
    // Multisampling
    // --------------------------------------------------------

    wgpu::MultisampleState multisampleState{};

    multisampleState.count = 1;
    multisampleState.mask = 0xFFFFFFFF;
    multisampleState.alphaToCoverageEnabled = false;


    // --------------------------------------------------------
    // Render pipeline
    // --------------------------------------------------------

    wgpu::RenderPipelineDescriptor pipelineDescriptor{};

    pipelineDescriptor.layout = nullptr;

    pipelineDescriptor.vertex = vertexState;
    pipelineDescriptor.primitive = primitiveState;
    pipelineDescriptor.multisample = multisampleState;
    pipelineDescriptor.fragment = &fragmentState;

    pipeline =
        device.CreateRenderPipeline(&pipelineDescriptor);

    std::cout << "WebGPU line pipeline created." << std::endl;

    return true;
}


// ------------------------------------------------------------
// Create or resize the GPU vertex buffer
// ------------------------------------------------------------

void WebGPURenderer::ensureVertexBuffer(
    uint64_t requiredBytes
) {

    if (requiredBytes <= vertexBufferCapacity) {
        return;
    }

    // Grow the buffer geometrically instead of reallocating
    // whenever the scene becomes slightly larger.
    uint64_t newCapacity = 256;

    while (newCapacity < requiredBytes) {
        newCapacity *= 2;
    }

    wgpu::BufferDescriptor bufferDescriptor{};

    bufferDescriptor.size = newCapacity;

    bufferDescriptor.usage =
        wgpu::BufferUsage::Vertex |
        wgpu::BufferUsage::CopyDst;

    bufferDescriptor.mappedAtCreation = false;

    vertexBuffer =
        device.CreateBuffer(&bufferDescriptor);

    vertexBufferCapacity = newCapacity;
}


// ------------------------------------------------------------
// Render the uploaded vertices
// ------------------------------------------------------------

void WebGPURenderer::render(
    const std::vector<WireVertex>& vertices
) {

    const uint64_t vertexCount = vertices.size();

    const uint64_t requiredBytes =
        vertexCount * sizeof(WireVertex);

    if (vertexCount > 0) {

        ensureVertexBuffer(requiredBytes);

        // Upload the CPU-generated vertex data.
        wgpu::Queue queue = device.GetQueue();

        queue.WriteBuffer(
            vertexBuffer,
            0,
            vertices.data(),
            requiredBytes
        );
    }


    // --------------------------------------------------------
    // Get the current canvas texture
    // --------------------------------------------------------

    wgpu::SurfaceTexture surfaceTexture{};

    surface.GetCurrentTexture(&surfaceTexture);

    if (
        surfaceTexture.status !=
            wgpu::SurfaceGetCurrentTextureStatus::SuccessOptimal
        &&
        surfaceTexture.status !=
            wgpu::SurfaceGetCurrentTextureStatus::SuccessSuboptimal
    ) {
        std::cerr << "Could not acquire WebGPU surface texture."
                  << std::endl;

        return;
    }

    wgpu::TextureView textureView =
        surfaceTexture.texture.CreateView();


    // --------------------------------------------------------
    // Render pass attachment
    // --------------------------------------------------------

    wgpu::RenderPassColorAttachment colourAttachment{};

    colourAttachment.view = textureView;
    colourAttachment.loadOp = wgpu::LoadOp::Clear;
    colourAttachment.storeOp = wgpu::StoreOp::Store;

    colourAttachment.clearValue.r = 0.05;
    colourAttachment.clearValue.g = 0.05;
    colourAttachment.clearValue.b = 0.07;
    colourAttachment.clearValue.a = 1.0;


    wgpu::RenderPassDescriptor renderPassDescriptor{};

    renderPassDescriptor.colorAttachmentCount = 1;
    renderPassDescriptor.colorAttachments = &colourAttachment;


    // --------------------------------------------------------
    // Encode rendering commands
    // --------------------------------------------------------

    wgpu::CommandEncoder encoder =
        device.CreateCommandEncoder();

    wgpu::RenderPassEncoder renderPass =
        encoder.BeginRenderPass(&renderPassDescriptor);

    renderPass.SetPipeline(pipeline);

    if (vertexCount > 0) {

        renderPass.SetVertexBuffer(
            0,
            vertexBuffer,
            0,
            requiredBytes
        );

        renderPass.Draw(
            static_cast<uint32_t>(vertexCount),
            1,
            0,
            0
        );
    }

    renderPass.End();


    // --------------------------------------------------------
    // Submit commands
    // --------------------------------------------------------

    wgpu::CommandBuffer commands = encoder.Finish();

    wgpu::Queue queue = device.GetQueue();

    queue.Submit(1, &commands);

    surface.Present();
}
