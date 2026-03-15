# TGSpeechBox as static libraries for mobile embedding

# --- libtgspeechbox.a (DSP engine) ---
add_library(tgspeechbox_static STATIC
    "${TGSB_DIR}/src/frame.cpp"
    "${TGSB_DIR}/src/speechPlayer.cpp"
    "${TGSB_DIR}/src/speechWaveGenerator.cpp"
)
target_include_directories(tgspeechbox_static PUBLIC "${TGSB_DIR}/src")
target_compile_features(tgspeechbox_static PRIVATE cxx_std_17)
set_property(TARGET tgspeechbox_static PROPERTY POSITION_INDEPENDENT_CODE ON)

if(NOT WIN32 AND NOT ANDROID)
    find_package(Threads REQUIRED)
    target_link_libraries(tgspeechbox_static PRIVATE Threads::Threads)
endif()

# --- libtgsbFrontend.a (IPA -> Frames) ---
file(GLOB FRONTEND_CPP CONFIGURE_DEPENDS "${TGSB_DIR}/src/frontend/*.cpp")
file(GLOB PASSES_CPP CONFIGURE_DEPENDS "${TGSB_DIR}/src/frontend/passes/*.cpp")

add_library(tgsbFrontend_static STATIC ${FRONTEND_CPP} ${PASSES_CPP})
target_include_directories(tgsbFrontend_static PUBLIC
    "${TGSB_DIR}/src"
    "${TGSB_DIR}/src/frontend"
)
target_compile_features(tgsbFrontend_static PRIVATE cxx_std_17)
target_compile_definitions(tgsbFrontend_static PUBLIC NVSP_FRONTEND_EXPORTS=1)
set_property(TARGET tgsbFrontend_static PROPERTY POSITION_INDEPENDENT_CODE ON)

# MSVC needs /utf-8 for IPA character literals (U'ˈ' etc.) in source files
if(MSVC)
    target_compile_options(tgspeechbox_static PRIVATE /utf-8)
    target_compile_options(tgsbFrontend_static PRIVATE /utf-8)
endif()
