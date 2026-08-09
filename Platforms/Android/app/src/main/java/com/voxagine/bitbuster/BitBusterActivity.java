package com.voxagine.bitbuster;

import org.libsdl.app.SDLActivity;

/**
 * The whole Java side of the game.
 *
 * SDLActivity does the work: it creates the surface, pumps the looper, and
 * dlopens the libraries getLibraries() names, then calls SDL_main in the last
 * of them. That is why the CMake target sets OUTPUT_NAME to "main" - the game
 * *is* libmain.so, and renaming it means overriding getLibraries() here.
 *
 * librttr_core and libc++_shared are not listed. They are pulled in as
 * NEEDED entries of libmain.so, so the dynamic linker resolves them without
 * being told; listing them would only matter if they were dlopen-only.
 */
public class BitBusterActivity extends SDLActivity
{
    @Override
    protected String[] getLibraries()
    {
        return new String[] {
            "SDL3",
            "main"
        };
    }
}
