#version 330 core
// SOURCEPORT: World-space shadow depth pass for geometry behind the camera.
// Accepts world-space XYZ directly — no camera-space reconstruction needed.
// Used when objects are behind the player and depth.vert cannot reconstruct
// their positions (e.g. tree canopy when the player turns away from the tree).

layout(location = 0) in vec3 aWorldPos;
layout(location = 1) in vec2 aTexCoord;

uniform mat4 uLightSpace;

smooth out vec2 vTexCoord;

void main() {
    gl_Position = uLightSpace * vec4(aWorldPos, 1.0);
    vTexCoord = aTexCoord;
}
