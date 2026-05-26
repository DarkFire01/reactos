add_executable(mcclone
    mcclone.c
    noise.c
    blocks.c
    texture.c
    world.c
    mesh.c
    player.c
    render.c
    items.c
    inventory.c
    game.c
    hud.c
    survival.c)
set_module_type(mcclone win32gui)
add_importlibs(mcclone opengl32 gdi32 user32 msvcrt kernel32)
add_cd_file(TARGET mcclone DESTINATION reactos/system32 FOR all)
