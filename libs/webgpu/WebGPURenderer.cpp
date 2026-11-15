#include "WebGPURenderer.h"

#include "Scene.h"
#include "Camera.h"
#include "Model.h"
#include "ModelTriangle.h"

#include <glm/gtc/matrix_inverse.hpp>

#include <iostream>
#include <vector>
#include <limits>
#include <utility>
#include <cstddef>
#include <cstdint>
#include <algorithm>

struct alignas(16) GpuRayTriangle {
    float p0[4];
    float p1[4];
    float p2[4];

    float n0[4];
    float n1[4];
    float n2[4];

    float faceNormal[4];

    float diffuse[4];
    float specular[4];

    // x = ambient
    // y = shininess
    // z = smooth shading enabled
    // w = emissive
    float params[4];
};

struct alignas(16) GpuLightSample {
    // xyz = position
    // w   = intensity
    float positionIntensity[4];

    // xyz = colour
    float colour[4];
};

struct alignas(16) GpuCameraData {
    float origin[4];
    float right[4];
    float up[4];
    float forward[4];

    // x = width
    // y = height
    // z = focal length multiplied by canvas scale
    // w = maximum ray distance
    float viewport[4];

    // x = triangle count
    // y = light sample count
    float counts[4];

    float background[4];
};

static void writeVec4(
    float destination[4],
    const glm::vec3& value,
    float w = 0.0f
) {
    destination[0] = value.x;
    destination[1] = value.y;
    destination[2] = value.z;
    destination[3] = w;
}

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

static const char* rasterShader = R"(
    struct VertexOutput {
        @builtin(position) position: vec4<f32>,
        @location(0) colour: vec3<f32>,
    };

    @vertex
    fn vertexMain(
        @location(0) position: vec3<f32>,
        @location(1) colour: vec3<f32>
    ) -> VertexOutput {

        var output: VertexOutput;

        output.position = vec4<f32>(
            position.x,
            position.y,
            position.z,
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

static const char* raytracingShader = R"(

struct Triangle {
    p0: vec4<f32>,
    p1: vec4<f32>,
    p2: vec4<f32>,

    n0: vec4<f32>,
    n1: vec4<f32>,
    n2: vec4<f32>,

    faceNormal: vec4<f32>,

    diffuse: vec4<f32>,
    specular: vec4<f32>,

    params: vec4<f32>,
};

struct LightSample {
    positionIntensity: vec4<f32>,
    colour: vec4<f32>,
};

struct CameraData {
    origin: vec4<f32>,
    right: vec4<f32>,
    up: vec4<f32>,
    forward: vec4<f32>,

    viewport: vec4<f32>,
    counts: vec4<f32>,
    background: vec4<f32>,
};

struct TriangleHit {
    distance: f32,
    u: f32,
    v: f32,
    valid: bool,
};

@group(0) @binding(0)
var<storage, read> triangles: array<Triangle>;

@group(0) @binding(1)
var<storage, read> lights: array<LightSample>;

@group(0) @binding(2)
var<uniform> camera: CameraData;


// ------------------------------------------------------------
// Full-screen triangle
// ------------------------------------------------------------

struct VertexOutput {
    @builtin(position) position: vec4<f32>,
};

@vertex
fn vertexMain(
    @builtin(vertex_index) vertexIndex: u32
) -> VertexOutput {

    var positions = array<vec2<f32>, 3>(
        vec2<f32>(-1.0, -1.0),
        vec2<f32>( 3.0, -1.0),
        vec2<f32>(-1.0,  3.0)
    );

    var output: VertexOutput;

    output.position = vec4<f32>(
        positions[vertexIndex],
        0.0,
        1.0
    );

    return output;
}


// ------------------------------------------------------------
// Ray / triangle intersection
// Moller-Trumbore algorithm
// ------------------------------------------------------------

fn intersectTriangle(
    origin: vec3<f32>,
    direction: vec3<f32>,
    triangle: Triangle
) -> TriangleHit {

    let edge1 = triangle.p1.xyz - triangle.p0.xyz;
    let edge2 = triangle.p2.xyz - triangle.p0.xyz;

    let p = cross(direction, edge2);

    let determinant = dot(edge1, p);

    if (abs(determinant) < 0.000001) {
        return TriangleHit(0.0, 0.0, 0.0, false);
    }

    let inverseDeterminant = 1.0 / determinant;

    let tVector = origin - triangle.p0.xyz;

    let u = dot(tVector, p) * inverseDeterminant;

    if (u < 0.0 || u > 1.0) {
        return TriangleHit(0.0, 0.0, 0.0, false);
    }

    let q = cross(tVector, edge1);

    let v = dot(direction, q) * inverseDeterminant;

    if (v < 0.0 || u + v > 1.0) {
        return TriangleHit(0.0, 0.0, 0.0, false);
    }

    let distance = dot(edge2, q) * inverseDeterminant;

    if (distance <= 0.001) {
        return TriangleHit(0.0, 0.0, 0.0, false);
    }

    return TriangleHit(distance, u, v, true);
}


// ------------------------------------------------------------
// Shadow test
// ------------------------------------------------------------

fn isOccluded(
    origin: vec3<f32>,
    direction: vec3<f32>,
    maxDistance: f32
) -> bool {

    let triangleCount = u32(camera.counts.x);

    for (var i = 0u; i < triangleCount; i = i + 1u) {

        let hit = intersectTriangle(
            origin,
            direction,
            triangles[i]
        );

        if (
            hit.valid &&
            hit.distance > 0.001 &&
            hit.distance < maxDistance - 0.002
        ) {
            return true;
        }
    }

    return false;
}


// ------------------------------------------------------------
// Fragment shader: one ray per pixel
// ------------------------------------------------------------

@fragment
fn fragmentMain(
    @builtin(position) pixel: vec4<f32>
) -> @location(0) vec4<f32> {

    let triangleCount = u32(camera.counts.x);

    // Generate a camera ray from the pixel position.
    let screenX = pixel.x - camera.viewport.x * 0.5;

    let screenY = camera.viewport.y * 0.5 - pixel.y;

    let rayOrigin = camera.origin.xyz;

    let rayDirection = normalize(
        screenX * camera.right.xyz +
        screenY * camera.up.xyz +
        camera.viewport.z * camera.forward.xyz
    );

    // No geometry: return the background.
    if (triangleCount == 0u) {
        return vec4<f32>(camera.background.xyz, 1.0);
    }

    // --------------------------------------------------------
    // Find closest intersection
    // --------------------------------------------------------

    var closestDistance = camera.viewport.w;

    var closestHit = TriangleHit(
        0.0,
        0.0,
        0.0,
        false
    );

    var closestTriangle = triangles[0];

    for (var i = 0u; i < triangleCount; i = i + 1u) {

        let hit = intersectTriangle(
            rayOrigin,
            rayDirection,
            triangles[i]
        );

        if (hit.valid && hit.distance < closestDistance) {

            closestDistance = hit.distance;

            closestHit = hit;

            closestTriangle = triangles[i];
        }
    }

    // --------------------------------------------------------
    // Background
    // --------------------------------------------------------

    if (!closestHit.valid) {
        return vec4<f32>(camera.background.xyz, 1.0);
    }

    // --------------------------------------------------------
    // Calculate hit information
    // --------------------------------------------------------

    let hitPosition =
        rayOrigin + rayDirection * closestDistance;

    let w = 1.0 - closestHit.u - closestHit.v;

    var normal = closestTriangle.faceNormal.xyz;

    // Interpolate vertex normals for smooth shading.
    if (closestTriangle.params.z > 0.5) {

        normal = normalize(
            w * closestTriangle.n0.xyz +
            closestHit.u * closestTriangle.n1.xyz +
            closestHit.v * closestTriangle.n2.xyz
        );
    }

    // Make the normal face towards the camera.
    if (dot(normal, rayDirection) > 0.0) {
        normal = -normal;
    }

    let viewDirection = -rayDirection;

    let baseColour = closestTriangle.diffuse.xyz;

    // --------------------------------------------------------
    // Emissive material
    // --------------------------------------------------------

    if (closestTriangle.params.w > 0.5) {
        return vec4<f32>(baseColour, 1.0);
    }

    // --------------------------------------------------------
    // Ambient lighting
    // --------------------------------------------------------

    var colour =
        baseColour * closestTriangle.params.x;

    // --------------------------------------------------------
    // Direct lighting and shadows
    // --------------------------------------------------------

    let lightCount = u32(camera.counts.y);

    for (var i = 0u; i < lightCount; i = i + 1u) {

        let light = lights[i];

        let toLight =
            light.positionIntensity.xyz - hitPosition;

        let distanceToLight = length(toLight);

        if (distanceToLight <= 0.00001) {
            continue;
        }

        let lightDirection =
            toLight / distanceToLight;

        // Shadow ray.
        let shadowOrigin =
            hitPosition + normal * 0.002;

        if (isOccluded(
            shadowOrigin,
            lightDirection,
            distanceToLight
        )) {
            continue;
        }

        // Diffuse component.
        let diffuseStrength =
            max(dot(normal, lightDirection), 0.0);

        // Specular component.
        let reflectedLight =
            reflect(-lightDirection, normal);

        let specularStrength = pow(
            max(dot(reflectedLight, viewDirection), 0.0),
            max(closestTriangle.params.y, 1.0)
        );

        // Distance attenuation.
        let attenuation =
            light.positionIntensity.w /
            (12.5663706 * distanceToLight * distanceToLight + 1.0);

        let diffuseColour =
            diffuseStrength * baseColour;

        let specularColour =
            specularStrength * closestTriangle.specular.xyz;

        colour +=
            (diffuseColour + specularColour) *
            light.colour.xyz *
            attenuation;
    }

    return vec4<f32>(
        clamp(colour, vec3<f32>(0.0), vec3<f32>(1.0)),
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

void WebGPURenderer::drawRasterised(
    const Scene& scene,
    const Camera& camera
) {
    if (!ready) return;

    std::vector<RasterVertex> vertices;

    const float canvasScale =
        static_cast<float>(height) / 2.0f;

    for (const auto& model : scene.models) {

        const glm::vec3 colour = model.material.diffuse;

        for (const auto& triangle : model.triangles) {

            const CanvasPoint a =
                camera.projectVertex(triangle[0], canvasScale);

            const CanvasPoint b =
                camera.projectVertex(triangle[1], canvasScale);

            const CanvasPoint c =
                camera.projectVertex(triangle[2], canvasScale);

            // Skip triangles behind the camera.
            if (a.depth <= 0.0f ||
                b.depth <= 0.0f ||
                c.depth <= 0.0f) {
                continue;
            }

            auto makeVertex = [&](const CanvasPoint& point) {

                RasterVertex vertex{};

                // Convert screen coordinates to NDC.
                vertex.position[0] =
                    (2.0f * point.x / width) - 1.0f;

                vertex.position[1] =
                    1.0f - (2.0f * point.y / height);

                // Convert reciprocal camera depth into
                // a WebGPU depth value.
                vertex.position[2] =
                    std::clamp(1.0f - point.depth, 0.0f, 1.0f);

                vertex.colour[0] = colour.r;
                vertex.colour[1] = colour.g;
                vertex.colour[2] = colour.b;

                return vertex;
            };

            // TriangleList: three vertices per triangle.
            vertices.push_back(makeVertex(a));
            vertices.push_back(makeVertex(b));
            vertices.push_back(makeVertex(c));
        }
    }

    render(vertices);
}

void WebGPURenderer::drawRaytraced(
    const Scene& scene,
    const Camera& camera
) {
    if (!ready) return;

    std::vector<GpuRayTriangle> gpuTriangles;
    std::vector<GpuLightSample> gpuLights;


    // --------------------------------------------------------
    // Convert scene triangles
    // --------------------------------------------------------

    for (const auto& model : scene.models) {

        const Material& material = model.material;

        for (const auto& triangle : model.triangles) {

            GpuRayTriangle gpu{};

            writeVec4(gpu.p0, triangle[0].position);
            writeVec4(gpu.p1, triangle[1].position);
            writeVec4(gpu.p2, triangle[2].position);

            writeVec4(gpu.n0, triangle[0].normal);
            writeVec4(gpu.n1, triangle[1].normal);
            writeVec4(gpu.n2, triangle[2].normal);

            writeVec4(gpu.faceNormal, triangle.normal);

            writeVec4(gpu.diffuse, material.diffuse);
            writeVec4(gpu.specular, material.specular);

            gpu.params[0] = material.ambient;
            gpu.params[1] = material.shininess;

            // FLAT uses the face normal.
            // GOURAUD and PHONG use interpolated vertex normals.
            gpu.params[2] =
                model.sMode == FLAT ? 0.0f : 1.0f;

            gpu.params[3] =
                material.emissive ? 1.0f : 0.0f;

            gpuTriangles.push_back(gpu);
        }
    }


    // --------------------------------------------------------
    // Convert light samples
    // --------------------------------------------------------

    for (const auto& light : scene.lights) {

        const size_t sampleCount =
            std::max<size_t>(1, light.samples.size());

        // A point light may have no generated sample positions.
        if (light.samples.empty()) {

            GpuLightSample gpu{};

            writeVec4(
                gpu.positionIntensity,
                light.position,
                light.intensity
            );

            gpu.positionIntensity[3] =
                light.intensity;

            writeVec4(gpu.colour, light.colour);

            gpuLights.push_back(gpu);
        }
        else {

            for (const auto& sample : light.samples) {

                GpuLightSample gpu{};

                writeVec4(
                    gpu.positionIntensity,
                    sample,
                    light.intensity /
                        static_cast<float>(sampleCount)
                );

                writeVec4(gpu.colour, light.colour);

                gpuLights.push_back(gpu);
            }
        }
    }


    // --------------------------------------------------------
    // Ensure GPU buffers are large enough
    // --------------------------------------------------------

    const uint64_t triangleBytes =
        gpuTriangles.size() * sizeof(GpuRayTriangle);

    const uint64_t lightBytes =
        gpuLights.size() * sizeof(GpuLightSample);

    ensureRayBuffers(triangleBytes, lightBytes);


    // --------------------------------------------------------
    // Upload scene data
    // --------------------------------------------------------

    wgpu::Queue queue = device.GetQueue();

    if (!gpuTriangles.empty()) {

        queue.WriteBuffer(
            rayTriangleBuffer,
            0,
            gpuTriangles.data(),
            triangleBytes
        );
    }

    if (!gpuLights.empty()) {

        queue.WriteBuffer(
            rayLightBuffer,
            0,
            gpuLights.data(),
            lightBytes
        );
    }


    // --------------------------------------------------------
    // Prepare camera data
    // --------------------------------------------------------

    GpuCameraData gpuCamera{};

    writeVec4(gpuCamera.origin, camera.position);

    // Match the ray generation in Camera::projectRay().
    const glm::mat3 inverseRotation =
        glm::inverse(camera.rotation);

    writeVec4(
        gpuCamera.right,
        glm::vec3(1, 0, 0) * inverseRotation
    );

    writeVec4(
        gpuCamera.up,
        glm::vec3(0, 1, 0) * inverseRotation
    );

    writeVec4(
        gpuCamera.forward,
        glm::vec3(0, 0, -1) * inverseRotation
    );

    const float canvasScale =
        static_cast<float>(height) / 2.0f;

    gpuCamera.viewport[0] = static_cast<float>(width);
    gpuCamera.viewport[1] = static_cast<float>(height);

    gpuCamera.viewport[2] =
        camera.focalLength * canvasScale;

    gpuCamera.viewport[3] = 20.0f;

    gpuCamera.counts[0] =
        static_cast<float>(gpuTriangles.size());

    gpuCamera.counts[1] =
        static_cast<float>(gpuLights.size());

    const glm::vec3 background =
        scene.backgroundColour(0, 0);

    writeVec4(gpuCamera.background, background);

    queue.WriteBuffer(
        rayCameraBuffer,
        0,
        &gpuCamera,
        sizeof(GpuCameraData)
    );


    // --------------------------------------------------------
    // Render
    // --------------------------------------------------------

    renderRaytraced();
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

    if (!createWireframePipeline()) {
        return false;
    }

    if (!createRasterisedPipeline()) {
        return false;
    }

    if (!createRaytracedPipeline()) {
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

    wgpu::TextureDescriptor depthDescriptor{};

    depthDescriptor.dimension = wgpu::TextureDimension::e2D;

    depthDescriptor.size.width =
        static_cast<uint32_t>(width);

    depthDescriptor.size.height =
        static_cast<uint32_t>(height);

    depthDescriptor.size.depthOrArrayLayers = 1;

    depthDescriptor.mipLevelCount = 1;
    depthDescriptor.sampleCount = 1;

    depthDescriptor.format = wgpu::TextureFormat::Depth24Plus;

    depthDescriptor.usage =
        wgpu::TextureUsage::RenderAttachment;

    depthTexture = device.CreateTexture(&depthDescriptor);

    depthTextureView = depthTexture.CreateView();

    return true;
}


// ------------------------------------------------------------
// Create the line-list render pipeline
// ------------------------------------------------------------

bool WebGPURenderer::createWireframePipeline() {

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


bool WebGPURenderer::createRasterisedPipeline() {

    // --------------------------------------------------------
    // Shader module
    // --------------------------------------------------------

    wgpu::ShaderSourceWGSL shaderSource{};

    shaderSource.code = rasterShader;

    wgpu::ShaderModuleDescriptor shaderDescriptor{};

    shaderDescriptor.nextInChain = &shaderSource;

    shaderModule =
        device.CreateShaderModule(&shaderDescriptor);


    // --------------------------------------------------------
    // Vertex attributes
    //
    // RasterVertex:
    //
    // float position[3];
    // float colour[3];
    //
    // Position = shader location 0
    // Colour   = shader location 1
    // --------------------------------------------------------

    wgpu::VertexAttribute attributes[2]{};

    attributes[0].format = wgpu::VertexFormat::Float32x3;
    attributes[0].offset = offsetof(RasterVertex, position);
    attributes[0].shaderLocation = 0;

    attributes[1].format = wgpu::VertexFormat::Float32x3;
    attributes[1].offset = offsetof(RasterVertex, colour);
    attributes[1].shaderLocation = 1;


    wgpu::VertexBufferLayout vertexBufferLayout{};

    vertexBufferLayout.arrayStride = sizeof(RasterVertex);
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
    // TriangleList fills each group of three vertices.
    // --------------------------------------------------------

    wgpu::PrimitiveState primitiveState{};

    primitiveState.topology =
        wgpu::PrimitiveTopology::TriangleList;

    primitiveState.stripIndexFormat =
        wgpu::IndexFormat::Undefined;

    primitiveState.frontFace =
        wgpu::FrontFace::CCW;

    primitiveState.cullMode =
        wgpu::CullMode::None;


    // --------------------------------------------------------
    // Depth testing
    // --------------------------------------------------------

    wgpu::DepthStencilState depthStencilState{};

    depthStencilState.format =
        wgpu::TextureFormat::Depth24Plus;

    depthStencilState.depthWriteEnabled =
        wgpu::OptionalBool::True;

    depthStencilState.depthCompare =
        wgpu::CompareFunction::Less;


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
    pipelineDescriptor.depthStencil = &depthStencilState;
    pipelineDescriptor.multisample = multisampleState;
    pipelineDescriptor.fragment = &fragmentState;

    pipeline =
        device.CreateRenderPipeline(&pipelineDescriptor);

    std::cout << "WebGPU rasterisation pipeline created."
              << std::endl;

    return true;
}


bool WebGPURenderer::createRaytracedPipeline() {

    // --------------------------------------------------------
    // Shader module
    // --------------------------------------------------------

    wgpu::ShaderSourceWGSL shaderSource{};

    shaderSource.code = raytracingShader;

    wgpu::ShaderModuleDescriptor shaderDescriptor{};

    shaderDescriptor.nextInChain = &shaderSource;

    rayShaderModule =
        device.CreateShaderModule(&shaderDescriptor);


    // --------------------------------------------------------
    // Bind group layout
    //
    // Binding 0: triangle storage buffer
    // Binding 1: light storage buffer
    // Binding 2: camera uniform buffer
    // --------------------------------------------------------

    wgpu::BindGroupLayoutEntry layoutEntries[3]{};

    layoutEntries[0].binding = 0;
    layoutEntries[0].visibility = wgpu::ShaderStage::Fragment;
    layoutEntries[0].buffer.type =
        wgpu::BufferBindingType::ReadOnlyStorage;

    layoutEntries[1].binding = 1;
    layoutEntries[1].visibility = wgpu::ShaderStage::Fragment;
    layoutEntries[1].buffer.type =
        wgpu::BufferBindingType::ReadOnlyStorage;

    layoutEntries[2].binding = 2;
    layoutEntries[2].visibility = wgpu::ShaderStage::Fragment;
    layoutEntries[2].buffer.type =
        wgpu::BufferBindingType::Uniform;


    wgpu::BindGroupLayoutDescriptor bindGroupLayoutDescriptor{};

    bindGroupLayoutDescriptor.entryCount = 3;
    bindGroupLayoutDescriptor.entries = layoutEntries;

    rayBindGroupLayout =
        device.CreateBindGroupLayout(&bindGroupLayoutDescriptor);


    // --------------------------------------------------------
    // Pipeline layout
    // --------------------------------------------------------

    wgpu::PipelineLayoutDescriptor pipelineLayoutDescriptor{};

    pipelineLayoutDescriptor.bindGroupLayoutCount = 1;
    pipelineLayoutDescriptor.bindGroupLayouts =
        &rayBindGroupLayout;

    rayPipelineLayout =
        device.CreatePipelineLayout(&pipelineLayoutDescriptor);


    // --------------------------------------------------------
    // Create GPU buffers
    // --------------------------------------------------------

    rayTriangleBufferCapacity = 256;
    rayLightBufferCapacity = 256;

    wgpu::BufferDescriptor triangleBufferDescriptor{};

    triangleBufferDescriptor.size = rayTriangleBufferCapacity;

    triangleBufferDescriptor.usage =
        wgpu::BufferUsage::Storage |
        wgpu::BufferUsage::CopyDst;

    rayTriangleBuffer =
        device.CreateBuffer(&triangleBufferDescriptor);


    wgpu::BufferDescriptor lightBufferDescriptor{};

    lightBufferDescriptor.size = rayLightBufferCapacity;

    lightBufferDescriptor.usage =
        wgpu::BufferUsage::Storage |
        wgpu::BufferUsage::CopyDst;

    rayLightBuffer =
        device.CreateBuffer(&lightBufferDescriptor);


    wgpu::BufferDescriptor cameraBufferDescriptor{};

    cameraBufferDescriptor.size = sizeof(GpuCameraData);

    cameraBufferDescriptor.usage =
        wgpu::BufferUsage::Uniform |
        wgpu::BufferUsage::CopyDst;

    rayCameraBuffer =
        device.CreateBuffer(&cameraBufferDescriptor);


    updateRayBindGroup();


    // --------------------------------------------------------
    // Vertex shader
    //
    // The full-screen triangle is generated using
    // vertex_index, so no vertex buffer is required.
    // --------------------------------------------------------

    wgpu::VertexState vertexState{};

    vertexState.module = rayShaderModule;
    vertexState.entryPoint = "vertexMain";
    vertexState.bufferCount = 0;
    vertexState.buffers = nullptr;


    // --------------------------------------------------------
    // Fragment shader
    // --------------------------------------------------------

    wgpu::ColorTargetState colourTarget{};

    colourTarget.format = surfaceFormat;
    colourTarget.writeMask = wgpu::ColorWriteMask::All;

    wgpu::FragmentState fragmentState{};

    fragmentState.module = rayShaderModule;
    fragmentState.entryPoint = "fragmentMain";
    fragmentState.targetCount = 1;
    fragmentState.targets = &colourTarget;


    // --------------------------------------------------------
    // Primitive state
    // --------------------------------------------------------

    wgpu::PrimitiveState primitiveState{};

    primitiveState.topology =
        wgpu::PrimitiveTopology::TriangleList;

    primitiveState.stripIndexFormat =
        wgpu::IndexFormat::Undefined;

    primitiveState.frontFace =
        wgpu::FrontFace::CCW;

    primitiveState.cullMode =
        wgpu::CullMode::None;


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

    pipelineDescriptor.layout = rayPipelineLayout;
    pipelineDescriptor.vertex = vertexState;
    pipelineDescriptor.primitive = primitiveState;
    pipelineDescriptor.multisample = multisampleState;
    pipelineDescriptor.fragment = &fragmentState;

    rayPipeline =
        device.CreateRenderPipeline(&pipelineDescriptor);

    std::cout << "WebGPU ray-tracing pipeline created."
              << std::endl;

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

void WebGPURenderer::updateRayBindGroup() {

    wgpu::BindGroupEntry entries[3]{};

    // Triangle buffer.
    entries[0].binding = 0;
    entries[0].buffer = rayTriangleBuffer;
    entries[0].offset = 0;
    entries[0].size = rayTriangleBufferCapacity;

    // Light buffer.
    entries[1].binding = 1;
    entries[1].buffer = rayLightBuffer;
    entries[1].offset = 0;
    entries[1].size = rayLightBufferCapacity;

    // Camera buffer.
    entries[2].binding = 2;
    entries[2].buffer = rayCameraBuffer;
    entries[2].offset = 0;
    entries[2].size = sizeof(GpuCameraData);


    wgpu::BindGroupDescriptor descriptor{};

    descriptor.layout = rayBindGroupLayout;
    descriptor.entryCount = 3;
    descriptor.entries = entries;

    rayBindGroup = device.CreateBindGroup(&descriptor);
}


void WebGPURenderer::ensureRayBuffers(
    uint64_t triangleBytes,
    uint64_t lightBytes
) {
    bool buffersChanged = false;

    // --------------------------------------------------------
    // Triangle buffer
    // --------------------------------------------------------

    if (triangleBytes > rayTriangleBufferCapacity) {

        uint64_t newCapacity = rayTriangleBufferCapacity;

        while (newCapacity < triangleBytes) {
            newCapacity *= 2;
        }

        wgpu::BufferDescriptor descriptor{};

        descriptor.size = newCapacity;

        descriptor.usage =
            wgpu::BufferUsage::Storage |
            wgpu::BufferUsage::CopyDst;

        rayTriangleBuffer =
            device.CreateBuffer(&descriptor);

        rayTriangleBufferCapacity = newCapacity;

        buffersChanged = true;
    }


    // --------------------------------------------------------
    // Light buffer
    // --------------------------------------------------------

    if (lightBytes > rayLightBufferCapacity) {

        uint64_t newCapacity = rayLightBufferCapacity;

        while (newCapacity < lightBytes) {
            newCapacity *= 2;
        }

        wgpu::BufferDescriptor descriptor{};

        descriptor.size = newCapacity;

        descriptor.usage =
            wgpu::BufferUsage::Storage |
            wgpu::BufferUsage::CopyDst;

        rayLightBuffer =
            device.CreateBuffer(&descriptor);

        rayLightBufferCapacity = newCapacity;

        buffersChanged = true;
    }


    // The bind group must reference the newly created buffers.
    if (buffersChanged) {
        updateRayBindGroup();
    }
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
}


void WebGPURenderer::render(
    const std::vector<RasterVertex>& vertices
) {

    const uint64_t vertexCount = vertices.size();

    const uint64_t requiredBytes =
        vertexCount * sizeof(RasterVertex);

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

    // Depth attachment.

    wgpu::RenderPassDepthStencilAttachment depthAttachment{};

    depthAttachment.view = depthTextureView;

    depthAttachment.depthLoadOp = wgpu::LoadOp::Clear;
    depthAttachment.depthStoreOp = wgpu::StoreOp::Store;

    depthAttachment.depthClearValue = 1.0f;
    depthAttachment.depthReadOnly = false;

    renderPassDescriptor.depthStencilAttachment =
        &depthAttachment;


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
}

void WebGPURenderer::renderRaytraced() {

    // --------------------------------------------------------
    // Acquire current canvas texture
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
    // Encode full-screen ray-tracing pass
    // --------------------------------------------------------

    wgpu::CommandEncoder encoder =
        device.CreateCommandEncoder();

    wgpu::RenderPassEncoder renderPass =
        encoder.BeginRenderPass(&renderPassDescriptor);

    renderPass.SetPipeline(rayPipeline);

    renderPass.SetBindGroup(0, rayBindGroup);

    // Draw the full-screen triangle.
    // The fragment shader traces one ray per pixel.
    renderPass.Draw(3, 1, 0, 0);

    renderPass.End();


    // --------------------------------------------------------
    // Submit
    // --------------------------------------------------------

    wgpu::CommandBuffer commands = encoder.Finish();

    wgpu::Queue queue = device.GetQueue();

    queue.Submit(1, &commands);
}
