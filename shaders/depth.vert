#version 330 core
// SOURCEPORT: World-space shadow depth pass (Path B).
// Screen-space sx/sy + sz (aDepth) carry enough information to reconstruct
// camera-space position.  A camera-to-world rotation (R^T) + camera world
// position then give the world-space position that the light-space matrix
// can project for the depth map.  Geometry with aDepth==0 (HUD, sky 2D
// overlays) is pushed outside NDC and clipped away.

layout(location = 0) in vec2  aPos;    // screen-space x, y (game pixels)
layout(location = 1) in float aDepth;  // sz = -16 / camera_z  (>0 for world geo)
layout(location = 4) in vec2  aTexCoord;

uniform mat4  uLightSpace;   // combined light view * proj matrix
uniform float uVideoCX;      // screen principal-point X
uniform float uVideoCY;      // screen principal-point Y
uniform float uCameraW;      // horizontal FOV scale  (CameraW)
uniform float uCameraH;      // vertical FOV scale    (CameraH)
uniform vec3  uCameraPos;    // camera world position
uniform mat3  uCamToWorld;   // camera-to-world rotation (R^T, column-major)

smooth out vec2 vTexCoord;

void main() {
    if (aDepth > 0.0) {
        // Reconstruct camera-space position from screen-space + depth.
        // sz = -16/cam_z  →  neg_cam_z = -cam_z = 16/sz
        float neg_cam_z = 16.0 / aDepth;
        float cam_x = (aPos.x - uVideoCX) * neg_cam_z / uCameraW;
        float cam_y = (uVideoCY - aPos.y) * neg_cam_z / uCameraH;
        vec3 camSpace = vec3(cam_x, cam_y, -neg_cam_z);  // cam_z is negative

        // Transform to world space and project with light matrix.
        vec3 worldPos = uCameraPos + uCamToWorld * camSpace;
        gl_Position = uLightSpace * vec4(worldPos, 1.0);
    } else {
        // HUD / sky / 2D overlays: push outside NDC so the rasteriser clips them.
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
    }
    vTexCoord = aTexCoord;
}
