// OptiScaler HUD-less marker
//
// This technique draws nothing. It only marks the point in the frame where the
// "OptiScaler HUD-less Capture" add-on copies the image for OptiScaler frame generation.
//
// Setup:
//  1. Enable OptiScaler_Hudless in ReShade and drag it to the BOTTOM of your technique list,
//     so it runs after every other effect in the same Toggler group.
//  2. In ReshadeEffectShaderToggler, make sure it renders in the group that runs right
//     before the game's UI (your "UI" group). If that group has "Allow all techniques"
//     on, it already does.

// Standalone marker: no shared shader package is required.
void VS_OptiScalerHudlessMarker(uint id : SV_VertexID,
    out float4 pos : SV_Position, out float2 texcoord : TEXCOORD)
{
    texcoord = float2((id << 1) & 2, id & 2);
    pos = float4(texcoord * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

void PS_OptiScalerHudlessMarker(float4 pos : SV_Position, float2 texcoord : TEXCOORD, out float4 color : SV_Target)
{
    color = 0;
    discard;
}

technique OptiScaler_Hudless <
    ui_label = "OptiScaler HUD-less marker";
    ui_tooltip = "Draws nothing. Marks where the OptiScaler HUD-less add-on captures the frame.\n"
                 "Keep it last in the list and in your Effect Toggler UI group.";
>
{
    pass
    {
        VertexShader = VS_OptiScalerHudlessMarker;
        PixelShader = PS_OptiScalerHudlessMarker;
    }
}
