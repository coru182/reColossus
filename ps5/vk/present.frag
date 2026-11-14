#version 450
layout(location = 0) in vec2 vUV;
layout(set = 0, binding = 0) uniform sampler2D uSrc;
layout(push_constant) uniform Push
{
    int uMode;
    int uFlip;
};
layout(location = 0) out vec4 oColor;
void main()
{
    vec4 c = texture(uSrc, vUV);
    // The display shows colour only: the picture's alpha is the GS's, not a coverage (as the host loop drew it).
    oColor = uMode == 0 ? vec4(c.rgb, 1.0) : c;
}
