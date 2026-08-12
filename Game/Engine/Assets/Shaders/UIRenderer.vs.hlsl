#include "Defines.hlsl"

static float3 vertices[] =
{
    float3(-1.0,	-1.0,	0.0),   //	Bottom Left		(-1,	-1)
    float3( 1.0,	-1.0,	0.0),   //	Bottom Right	( 1, 	-1)
    float3(-1.0,	 1.0,	0.0),   //	Top Left		(-1,	 1)
    float3( 1.0,	 1.0,	0.0)    //	Top Right		( 1,	 1)
};

static float2 AlignmentCoords[9] =
{
	float2(0.0, 0.0),	// Centered
	float2(-1.0, 1.0),	// TopLeft
	float2(0.0, 1.0),	// TopCenter
	float2(1.0, 1.0),	// TopRight
	float2(1.0, 0.0),	// RightCenter
	float2(-1.0, 0.0),	// LeftCenter
	float2(-1.0, -1.0),	// BottomLeft
	float2(0.0, -1.0),	// BottomCenter
	float2(1.0, -1.0),	// BottomRight
};

// Register (b0)
#include "CameraData.hlsl"

struct SpriteData
{
	matrix Model;
	uint TextureID;

	// Keep this structured-buffer ABI byte-for-byte identical to the C++
	// SpriteData. Vector members here are otherwise padded to 16-byte
	// boundaries by SPIR-V, while GLM's Vector2/Vector4 members are packed.
	float ColorR;
	float ColorG;
	float ColorB;
	float ColorA;

	float OffsetX;
	float OffsetY;
	float SizeX;
	float SizeY;
	
	uint Alignment;
	uint ScreenAlignment;
	
	uint IsScreen;
	
	int Layer;
	
	float TextureRepeatX;
	float TextureRepeatY;

	float CullStartX;
	float CullStartY;
	float CullEndX;
	float CullEndY;
	
	uint padding;
};

struct TextData
{
	uint FontID;
	uint CharID;
	matrix Model;
};

STRUCTURED_BUFFER(SpriteData) Sprites : register(t0);

struct VS_out
{
    float4	Position	: POS_OUT;
    float2	UVs			: TEXCOORD0;
    uint 	TextureID 	: POSITION;
	float4	Color		: COLOR;
};

VS_out main(uint IDvert : VERT_ID, uint IDinst : INST_ID)
{
	VS_out OUT;
	SpriteData sprite = Sprites[IDinst];
	float2 spriteSize = float2(sprite.SizeX, sprite.SizeY);
	float2 textureRepeat = float2(sprite.TextureRepeatX, sprite.TextureRepeatY);
	float4 spriteColor = float4(sprite.ColorR, sprite.ColorG, sprite.ColorB, sprite.ColorA);
	uint alignment = min(sprite.Alignment, 8u);
	uint screenAlignment = min(sprite.ScreenAlignment, 8u);
	
	// Convert cull start and end from 0,0 - 1,1 to -1,-1 - 1,1
	float2 cullStart = float2(sprite.CullStartX, sprite.CullStartY) * 2 - 1;
	float2 cullEnd = float2(sprite.CullEndX, sprite.CullEndY) * 2 - 1;
	
	// Clamp vertex positions with culling.
	float3 vertexPos = vertices[IDvert];
	vertexPos.xy = clamp(vertexPos.xy, cullStart, cullEnd);
	
	float3 normCoords = vertexPos - float3(AlignmentCoords[alignment], 0.0); // Setting the vertex position with correct center.
	float3 spriteCoords = normCoords * float3(spriteSize * 0.5, 1.0); // Sizing mesh to the image size
	
	if (sprite.IsScreen)
	{
		float2 screenCoords = spriteCoords.xy;
		screenCoords = mul(sprite.Model, float4(screenCoords, 0.0, 1.0)).xy;
		
		OUT.Position.xy = (screenCoords / (viewport.xy * 0.5) - float2(1.0, 1.0)) + (AlignmentCoords[screenAlignment] + float2(1.0, 1.0));
		// Convert layer from -9999 - 9999 to 0 - 1 for depth.
		OUT.Position.z = ((float(sprite.Layer) * 0.0001) + 1) * 0.5;
		OUT.Position.w = 1.0;
	}
	else
	{
		float3 worldCoords = float3(spriteCoords.xy, 0.0);
		OUT.Position = mul(mvp, mul(sprite.Model, float4(worldCoords, 1.0)));
	}
	
	OUT.UVs = (vertexPos.xy + float2(1.0, 1.0)) * 0.5; // Convert from -1,-1 - 1,1 To 0,0 - 1,1
	OUT.UVs.y = 1.0 - OUT.UVs.y; // Invert Y axis
	
	OUT.UVs *= textureRepeat; // Repeat sprite
	
	OUT.TextureID = sprite.TextureID;
	OUT.Color = spriteColor;
	
	return OUT;
}
