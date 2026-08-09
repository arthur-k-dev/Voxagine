#ifndef VOXAGINE_COLOR_HLSL
#define VOXAGINE_COLOR_HLSL

/* sRGB <-> linear, and where the boundary between them is - RENDERING_PLAN.md
   phase 7.2 (which is 6.4).

   **What is stored is encoded; what is lit is linear.** Every colour this
   engine keeps is 8-bit sRGB: the .vox palettes are authored in MagicaVoxel,
   the sprites are PNGs, and the render targets are R8G8B8A8_UNORM holding
   values the compositor will interpret as sRGB when the present blit copies
   them out. None of that changes here. What changes is that the *arithmetic*
   in between - N.L, the sum of two lights, an occlusion multiplier, and every
   bounce term phase 7.3 adds - now happens on linear radiance, with a decode
   where albedo is read and an encode where the scene target is written.

   **Why not linear all the way to present, which is how 6.4 was written.**
   That would mean the scene target holds linear values, and three things
   downstream of it are wrong then rather than right:

     - FXAA is a *perceptual* edge filter. It computes luma from the encoded
       value and thresholds on it; run on linear input it under-detects edges
       in shadow and over-detects them in highlights.
     - The UI target, ImGui and the debug lines all composite into the same
       image, and all three are authored sRGB. They would each need decoding
       on the way in and the blend would change with them.
     - 8-bit linear bands visibly in the darks. sRGB's curve exists precisely
       to spend those codes where the eye is; a linear 8-bit target throws that
       away and the fix is an FP16 target, which is 2x the bandwidth of the
       largest target in the frame at 4K.

   So the encode sits at the end of scene *shading* rather than at present. The
   region between SrgbToLinear on albedo and EncodeSceneColor on the way out is
   the linear pipeline, and it is exactly the region 7.3's bounce light needs.

   The exact piecewise sRGB transfer function rather than pow(x, 2.2): the
   present blit is a byte copy into a surface the compositor reads as sRGB, so
   sRGB is what the display actually applies, and matching it is free here. */

/* Decode a stored colour - a voxel's palette entry, a texel - to linear. */
float3 SrgbToLinear(float3 v3Srgb)
{
	float3 v3Clamped = max(v3Srgb, 0.0);

	float3 v3Low = v3Clamped / 12.92;
	float3 v3High = pow((v3Clamped + 0.055) / 1.055, 2.4);

	return lerp(v3Low, v3High, step(0.04045, v3Clamped));
}

/* Encode linear radiance for storage in an 8-bit target. Deliberately does not
   clamp the top end: a value over 1 is an overbright the target write saturates,
   which is what happened before this phase too. */
float3 LinearToSrgb(float3 v3Linear)
{
	float3 v3Clamped = max(v3Linear, 0.0);

	float3 v3Low = v3Clamped * 12.92;
	float3 v3High = 1.055 * pow(v3Clamped, 1.0 / 2.4) - 0.055;

	return lerp(v3Low, v3High, step(0.0031308, v3Clamped));
}

/* The one place the linear region ends. Named rather than calling LinearToSrgb
   directly at the four shading sites, so that a grep for it lists the whole
   boundary. */
float3 EncodeSceneColor(float3 v3Linear)
{
	return LinearToSrgb(v3Linear);
}

/* A multiplier that was tuned against the old gamma-space result, converted so
   it lands the same way on linear radiance. Applying a 1.02 gain to a linear
   value and then encoding it is worth roughly 1.009 on screen - the effect
   silently loses half its strength otherwise.

   2.2 rather than 2.4 on purpose: this approximates the *whole* sRGB curve
   including its linear toe, and 2.2 is the standard fit for that. It is a
   compatibility shim for constants that predate this phase, not a colour
   conversion - anything new should be authored as a linear gain. */
float GammaGainToLinear(float fGain)
{
	return pow(max(fGain, 0.0), 2.2);
}

#endif
