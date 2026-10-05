# rmw::video — the video wallpapers' plan (core -> rmw-wallpaper) and the decisions rmw-wallpaper
# makes without Windows: when to pause, where the frame goes, when the next one is due (docs/04 M6).

rmw_add_library(video
    SOURCES
        src/Plan.cpp
        src/Playback.cpp
    PUBLIC_DEPS
        rmw::base
        rmw::domain
        rmw::json
)
