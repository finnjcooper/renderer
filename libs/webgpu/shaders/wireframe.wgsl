struct VertexOutput {
    @builtin(position) position: vec4f,
    @location(0) colour: vec3f,
};

@vertex
fn vertexMain(
    @location(0) position: vec2f,
    @location(1) colour: vec3f
) -> VertexOutput {
    var output: VertexOutput;

    output.position = vec4f(position, 0.5, 1.0);
    output.colour = colour;

    return output;
}

@fragment
fn fragmentMain(
    input: VertexOutput
) -> @location(0) vec4f {
    return vec4f(input.colour, 1.0);
}
