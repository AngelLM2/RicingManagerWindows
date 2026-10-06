# rmw::video — the live wallpapers' plan (core -> rmw-wallpaper) and the decisions rmw-wallpaper
# makes without Windows: when to pause, where the frame goes, when the next one is due (docs/04 M6),
# the colors of the effects and the motion of the particles (M12).

rmw_add_library(video
    SOURCES
        src/Effects.cpp
        src/Particles.cpp
        src/Plan.cpp
        src/Playback.cpp
    PUBLIC_DEPS
        rmw::base
        rmw::domain
        rmw::json
)
