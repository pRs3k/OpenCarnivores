#version 330 core

// SOURCEPORT: deferred sky reconstruction shader for VR.
// Reconstructs the flatscreen sky appearance from view direction without geometry,
// naturally excluding roll so the sky stays stable during head tilt.

noperspective in vec2 vTexCoord;  // Screen coordinates in [0, WinW] x [0, WinH]

uniform float uVideoCX;           // Principal point X (pixels)
uniform float uVideoCY;           // Principal point Y (pixels)
uniform float uCameraW;           // Focal length X (pixels)
uniform float uCameraH;           // Focal length Y (pixels)
uniform vec3  uCameraPos;         // World camera position
uniform mat3  uCamToWorld;        // Camera-to-world rotation (yaw/pitch only, no roll)
uniform sampler2D uSkyTexture;
uniform float uSkyTime;           // Cloud animation time
uniform float uWinW;
uniform float uWinH;

out vec4 fragColor;

void main() {
    // Screen coordinates to normalized device coordinates
    // vTexCoord is in pixel space [0, WinW] x [0, WinH]
    // Convert to NDC [-1, 1] x [-1, 1]
    float ndc_x = 2.0 * vTexCoord.x / uWinW - 1.0;
    float ndc_y = 1.0 - 2.0 * vTexCoord.y / uWinH;  // Flip Y for OpenGL

    // Unproject from screen coordinates to view direction.
    // The orthographic projection maps:
    //   view_x in [-CameraW/2, CameraW/2] -> ndc_x in [-1, 1]
    //   view_y in [-CameraH/2, CameraH/2] -> ndc_y in [-1, 1]
    // But the principal point is offset: VideoCX, VideoCY
    // So: screen_x maps to view direction x = (screen_x - VideoCX) / focal_x
    float view_x = (vTexCoord.x - uVideoCX) / uCameraW;
    float view_y = (uVideoCY - vTexCoord.y) / uCameraH;  // Flip Y for game convention

    // View direction in camera space: looking down -Z with (X,Y) offset
    // Normalize for proper direction
    vec3 view_dir = normalize(vec3(view_x, view_y, -1.0));

    // SOURCEPORT: apply full camera-to-world rotation (yaw and pitch, no roll)
    vec3 world_dir = uCamToWorld * view_dir;

    // Project onto infinite sky plane at distance 100000 for UV computation
    vec3 world_pos = uCameraPos + world_dir * 100000.0;

    // Reconstruct the UV mapping from RenderSkyPlane math (line 6152-6156).
    // RenderSkyPlane uses: vbase.x = -CameraX / 128.f; vbase.z = +CameraZ / 128.f;
    // Then applies RotateVVector to rotate by camera orientation.
    // Our world_pos is already in world space, so use it directly.
    float tu_base = -world_pos.x / 128.0;
    float tv_base = world_pos.z / 128.0;

    // Compute texture UVs: animate by time like RenderSkyPlane (line 6228)
    // tu, tv are scaled by 1/256 in the original code (line 6228: (tu + dtt) / 256.f)
    float dtt = mod(uSkyTime / 512.0, 1.0);
    float tu = (tu_base + dtt) / 256.0;
    float tv = (tv_base - dtt) / 256.0;

    // SOURCEPORT: DEBUG - simple solid sky color
    fragColor = vec4(0.5, 0.7, 1.0, 1.0);  // Light blue sky
}
