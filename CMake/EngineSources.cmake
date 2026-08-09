# Engine translation units.
#
# Generated from the tree and then maintained by hand: the Linux port adds and
# removes files often enough that a GLOB would silently pick up half-ported
# code. Keep this list sorted.

set(VOXAGINE_ENGINE_SOURCES
    ${VOXAGINE_SOURCE_DIR}/Core/Application.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/Platform/Input/SDL/SDLGamePad.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/Platform/Input/SDL/SDLKeyboard.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/Platform/Input/SDL/SDLMouse.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/ECS/Component.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/ECS/Components/AudioPlaylist.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/ECS/Components/AudioSource.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/ECS/Components/BehaviorScript.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/ECS/Components/BoxCollider.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/ECS/Components/ChunkViewer.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/ECS/Components/Collider.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/ECS/Components/InputHandler.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/ECS/Components/Particles/Emitters/BoxEmitter.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/ECS/Components/Particles/Emitters/SphereEmitter.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/ECS/Components/Particles/Emitters/VoxFrameEmitter.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/ECS/Components/Particles/Modules/AttractorModule.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/ECS/Components/Particles/Modules/BasicTimerModule.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/ECS/Components/Particles/Modules/CollisionModule.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/ECS/Components/Particles/ParticleEmitter.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/ECS/Components/Particles/ParticlePool.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/ECS/Components/Particles/ParticleSystem.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/ECS/Components/PhysicsBody.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/ECS/Components/SpriteRenderer.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/ECS/Components/TextRenderer.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/ECS/Components/Transform.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/ECS/Components/UI/UIButton.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/ECS/Components/UI/UIComponent.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/ECS/Components/UI/UISlider.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/ECS/Components/VoxAnimator.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/ECS/Components/VoxRenderer.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/ECS/ComponentSystem.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/ECS/Entities/Camera.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/ECS/Entities/UI/Canvas.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/ECS/Entities/ViewPoint.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/ECS/Entity.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/ECS/Systems/AudioSystem.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/ECS/Systems/Chunk/Chunk.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/ECS/Systems/Chunk/ChunkSystem.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/ECS/Systems/Chunk/FarFieldBaker.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/ECS/Systems/Chunk/ChunkUpdateGroup.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/ECS/Systems/Pathfinding/Grid/PathfindingChunk.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/ECS/Systems/Pathfinding/Grid/PathfindingChunkGrid.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/ECS/Systems/Pathfinding/Grid/PathfindingNode.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/ECS/Systems/Pathfinding/Grid/PathfindingObstacle.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/ECS/Systems/Pathfinding/Jobs/ChunkBuilderJob.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/ECS/Systems/Pathfinding/Navigation/ContinuumCrowdsGroup.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/ECS/Systems/Pathfinding/Navigation/Pathfinder.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/ECS/Systems/Pathfinding/Navigation/PathfinderGoal.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/ECS/Systems/Pathfinding/Navigation/PathfinderGroup.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/ECS/Systems/Physics/Box.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/ECS/Systems/Physics/IntegrityChecker.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/ECS/Systems/Physics/PhysicsSystem.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/ECS/Systems/Physics/Sphere.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/ECS/Systems/Physics/VoxelGrid.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/ECS/Systems/Rendering/DebugRenderer.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/ECS/Systems/Rendering/RenderSystem.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/ECS/Systems/Rendering/VoxelBaker.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/ECS/Systems/ScriptSystem.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/ECS/World.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/ECS/WorldManager.cpp

    # Both of these shipped as a header plus a Windows .lib, so there was
    # nothing to link against. TeenyPath is reimplemented on std::filesystem
    # and is portable; the file dialogs are per-platform and selected below.
    ${VOXAGINE_SOURCE_DIR}/External/teenypath/teenypath.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/JsonSerializer.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/System/MobileAssets.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/System/MobileLog.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/LoggingSystem/LoggingSystem.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/Memory/Allocators/BaseAlloc.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/Memory/Allocators/FreeListAlloc.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/Memory/Allocators/LinearAlloc.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/Memory/Allocators/PoolAlloc.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/Memory/Allocators/StackAlloc.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/Objects/TSubclass.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/Objects/VClass.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/Platform/Audio/AudioContext.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/Platform/Audio/NullAudioContext.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/Resources/Formats/PlatformSoundReference.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/Platform/Input/InputContext.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/Platform/Input/Temp/GamePadController.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/Platform/Input/Temp/InputBindingAction.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/Platform/Input/Temp/InputBindingAxis.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/Platform/Input/Temp/InputBindingBase.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/Platform/Input/Temp/InputBindingHandlerInterface.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/Platform/Input/Temp/InputBindingMap.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/Platform/Input/Temp/InputBindingMapInterface.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/Platform/Input/Temp/InputContextNew.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/Platform/Input/Temp/InputController.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/Platform/Input/Temp/KeyboardController.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/Platform/Input/Temp/KeyboardControllerInterface.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/Platform/Input/Temp/MouseController.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/Platform/Input/Temp/MouseControllerInterface.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/Platform/Input/Temp/PlayerController.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/Platform/Input/Temp/TouchController.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/Platform/Platform.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/Platform/Rendering/CommandEngine.cpp
    # FrameProfiler.cpp lives in the voxagine_vulkan target instead (see
    # CMakeLists.txt) - voxagine links that publicly, so it's still callable
    # from here, without compiling it into both libraries.
    ${VOXAGINE_SOURCE_DIR}/Core/Platform/Rendering/Managers/IDManager.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/Platform/Rendering/Managers/ModelManager.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/Platform/Rendering/Managers/TextureManager.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/Platform/Rendering/Objects/Buffer.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/Platform/Rendering/Passes/DebugPass.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/Platform/Rendering/Passes/ParticlePass.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/Platform/Rendering/Passes/PostProcessingPass.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/Platform/Rendering/Passes/UIPass.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/Platform/Rendering/Passes/VoxelBakePass.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/Platform/Rendering/Passes/SunShadowPass.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/Platform/Rendering/Passes/VoxelPass.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/Platform/Rendering/RenderContext.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/Platform/Rendering/VoxelBrickGrid.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/Platform/Rendering/FarFieldVolume.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/Platform/Rendering/Vulkan/Managers/VKModelManager.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/Platform/Rendering/Vulkan/Managers/VKTextureManager.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/Platform/Rendering/Vulkan/VKRenderContext.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/Platform/Rendering/Vulkan/VKComputePass.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/Platform/Rendering/Vulkan/VKRenderPass.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/Platform/Rendering/Vulkan/Objects/VKBuffer.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/Platform/Rendering/Vulkan/Objects/VKMapper.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/Platform/Rendering/Vulkan/Objects/VKSampler.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/Platform/Rendering/Vulkan/Objects/VKShader.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/Platform/Rendering/Vulkan/Objects/VKView.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/Platform/Time/Chrono/ChronoGameTimer.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/Platform/Window/SDL/SDLWindowContext.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/Platform/Window/WindowContext.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/PlayerPrefs/PlayerPrefs.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/Resources/Formats/ShaderReference.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/Resources/Formats/TextureReference.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/Resources/Formats/VoxModel.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/Resources/ResourceManager.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/Particles/ParticleCore.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/LaunchOptions.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/Settings.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/Voxels/VoxelEditBatch.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/System/Posix/PosixFileSystem.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/Threading/Job.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/Threading/JobManager.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/Threading/JobQueue.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/Threading/JobThread.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/Utils/DataHook/DataHook.cpp
    ${VOXAGINE_SOURCE_DIR}/Core/Utils/Utils.cpp
    ${VOXAGINE_SOURCE_DIR}/Editor/Configuration/BaseSettings.cpp
    ${VOXAGINE_SOURCE_DIR}/Editor/Configuration/Project/ProjectSettings.cpp
    ${VOXAGINE_SOURCE_DIR}/Editor/imgui/ImguiSystem.cpp
    ${VOXAGINE_SOURCE_DIR}/Editor/imgui/Contexts/VKImContext.cpp
    ${VOXAGINE_SOURCE_DIR}/Editor/imgui/Platforms/SDLImPlatform.cpp
    ${VOXAGINE_SOURCE_DIR}/pch.cpp

    # Vendored sources compiled into the engine.
    #
    # glm is header-only. External/glm/detail/glm.cpp is its optional static
    # library TU and includes <glm/...> unqualified, which resolves to the
    # system glm 1.x and collides with the vendored 0.9.x. Never add it back.
    ${VOXAGINE_SOURCE_DIR}/External/imgui/imgui.cpp
    ${VOXAGINE_SOURCE_DIR}/External/imgui/imgui_demo.cpp
    ${VOXAGINE_SOURCE_DIR}/External/imgui/imgui_draw.cpp
    ${VOXAGINE_SOURCE_DIR}/External/imgui/imgui_dropdown.cpp
    # imgui_stl.cpp is the same upstream file under its older name and defines
    # the same two symbols, so building both is a duplicate definition. It went
    # unnoticed because the linker simply dropped whichever object nothing
    # referenced; forcing the archive in whole (see the root CMakeLists) turns
    # it into the link error it always was. The stale header is still included
    # in three editor files and still works - it declares what imgui_stdlib.cpp
    # defines - so only the source is dropped here.
    ${VOXAGINE_SOURCE_DIR}/External/imgui/imgui_stdlib.cpp
    ${VOXAGINE_SOURCE_DIR}/External/imgui/imgui_widgets.cpp
    ${VOXAGINE_SOURCE_DIR}/External/imguizmo/ImGuizmo.cpp
    ${VOXAGINE_SOURCE_DIR}/External/imguizmo/ImSequencer.cpp
    ${VOXAGINE_SOURCE_DIR}/External/STB/image_DXT.c
    ${VOXAGINE_SOURCE_DIR}/External/STB/image_helper.c
    ${VOXAGINE_SOURCE_DIR}/External/STB/stb_image_aug.c
)

# The audio backend, selected by VOXAGINE_AUDIO_BACKEND. miniaudio is vendored
# and header-only, so there is nothing to find or link - unlike FMOD, which
# this list used to name and which had no library anywhere in the tree to link
# against, so turning it on would have failed at the link step.
#
# NullAudioContext is not here: it is in the list above and always compiles,
# because AA_NONE selects it at runtime whatever the build did.
set(VOXAGINE_MINIAUDIO_SOURCES
    ${VOXAGINE_SOURCE_DIR}/Core/Platform/Audio/MiniaudioContext.cpp
    ${VOXAGINE_SOURCE_DIR}/External/miniaudio/miniaudio_impl.c
    ${VOXAGINE_SOURCE_DIR}/External/miniaudio/stb_vorbis.c
)

# The editor, and the desktop file dialog it is the only user of.
#
# These are appended to VOXAGINE_ENGINE_SOURCES only when VOXAGINE_BUILD_EDITOR
# is on, which used not to be true of anything: every Editor/*.cpp compiled
# into every build, and Editor.cpp has no #ifdef EDITOR around it, so a *game*
# binary carried the whole editor - and with it FileBrowser, and with that
# nfd_portal.cpp, which needs <poll.h> and a desktop D-Bus portal. A mobile
# game target cannot compile that file at all.
#
# Four Editor/ translation units are deliberately NOT here, because a game
# build genuinely needs them:
#   imgui/*                     - the game draws its debug text through ImGui.
#   Configuration/BaseSettings  - PlayerPrefs derives from it.
#   Configuration/Project/ProjectSettings - VoxApp reads it in the
#                                 *non*-editor branch, to find the start world.
set(VOXAGINE_EDITOR_SOURCES
    ${VOXAGINE_SOURCE_DIR}/Core/FileBrowser.cpp
    ${VOXAGINE_SOURCE_DIR}/Editor/Configuration/ConfigurationWindow.cpp
    ${VOXAGINE_SOURCE_DIR}/Editor/Configuration/Editor/EditorConfigurationWindow.cpp
    ${VOXAGINE_SOURCE_DIR}/Editor/Configuration/Editor/UserSettings.cpp
    ${VOXAGINE_SOURCE_DIR}/Editor/Configuration/Project/ProjectConfigurationWindow.cpp
    ${VOXAGINE_SOURCE_DIR}/Editor/Configuration/WorldCreateConfig.cpp
    ${VOXAGINE_SOURCE_DIR}/Editor/ConsoleLog/ConsoleLog.cpp
    ${VOXAGINE_SOURCE_DIR}/Editor/EditorButton.cpp
    ${VOXAGINE_SOURCE_DIR}/Editor/EditorCamera.cpp
    ${VOXAGINE_SOURCE_DIR}/Editor/Editor.cpp
    ${VOXAGINE_SOURCE_DIR}/Editor/EditorReferenceManager.cpp
    ${VOXAGINE_SOURCE_DIR}/Editor/EditorRenderMapper.cpp
    ${VOXAGINE_SOURCE_DIR}/Editor/EditorWorld.cpp
    ${VOXAGINE_SOURCE_DIR}/Editor/EntityHierarchy/EntityHierarchy.cpp
    ${VOXAGINE_SOURCE_DIR}/Editor/EntityInspector/EntityInspector.cpp
    ${VOXAGINE_SOURCE_DIR}/Editor/EntityWizard/EntityWizard.cpp
    ${VOXAGINE_SOURCE_DIR}/Editor/PropertyRenderer/PropertyRenderer.cpp
    ${VOXAGINE_SOURCE_DIR}/Editor/PropertyRenderer/TMap.cpp
    ${VOXAGINE_SOURCE_DIR}/Editor/SnappingTool/SnappingTool.cpp
    ${VOXAGINE_SOURCE_DIR}/Editor/UndoRedo/CommandManager.cpp
    ${VOXAGINE_SOURCE_DIR}/Editor/UndoRedo/EditorComponentCommand.cpp
    ${VOXAGINE_SOURCE_DIR}/Editor/UndoRedo/EditorEntityChildCommand.cpp
    ${VOXAGINE_SOURCE_DIR}/Editor/UndoRedo/EditorEntityCommand.cpp
    ${VOXAGINE_SOURCE_DIR}/Editor/UndoRedo/EditorFunctionCommand.cpp
    ${VOXAGINE_SOURCE_DIR}/Editor/UndoRedo/EditorPropertyCommand.cpp
    ${VOXAGINE_SOURCE_DIR}/Editor/UndoRedo/EditorSelectedEntityCommand.cpp
    ${VOXAGINE_SOURCE_DIR}/Editor/UndoRedo/EditorTransformMatrixCommand.cpp
    ${VOXAGINE_SOURCE_DIR}/Editor/Window.cpp
)

# File dialogs are the one piece of the editor with no portable implementation:
# Win32 has GetOpenFileName, and on Linux we shell out to whatever portal-aware
# dialog the desktop provides rather than taking a GTK dependency.
if(WIN32)
    list(APPEND VOXAGINE_EDITOR_SOURCES
        ${VOXAGINE_SOURCE_DIR}/External/nativefiledialog/nfd_win32.cpp)
elseif(NOT ANDROID AND NOT IOS)
    list(APPEND VOXAGINE_EDITOR_SOURCES
        ${VOXAGINE_SOURCE_DIR}/External/nativefiledialog/nfd_portal.cpp)
endif()
