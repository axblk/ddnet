fn main() {
    cc::Build::new()
        .cpp(true)
        .std("c++20")
        .include("../src")
        .file("../src/engine/shared/game_wire.cpp")
        .file("cpp/game_wire_ffi.cpp")
        .compile("ddnet_game_wire_ffi");
    println!("cargo:rerun-if-changed=cpp/game_wire_ffi.cpp");
    println!("cargo:rerun-if-changed=../src/engine/shared/game_wire.cpp");
    println!("cargo:rerun-if-changed=../src/engine/shared/game_wire.h");
}
