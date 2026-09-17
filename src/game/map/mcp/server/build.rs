// Links the map tools' shared library, which CMake builds as
// `libddnet_map_mcp.so` (target `map-mcp-core`). Its directory comes in as
// `DDNET_MAP_MCP_LIB_DIR`, and the program remembers it as its rpath so that
// it runs from wherever it was copied to.

use std::env;

fn main() {
    println!("cargo:rerun-if-env-changed=DDNET_MAP_MCP_LIB_DIR");
    let dir = env::var("DDNET_MAP_MCP_LIB_DIR").unwrap_or_else(|_| {
        panic!("DDNET_MAP_MCP_LIB_DIR has to name the directory holding libddnet_map_mcp (build the CMake target map-mcp-core first)")
    });
    println!("cargo:rustc-link-search=native={dir}");
    println!("cargo:rustc-link-lib=dylib=ddnet_map_mcp");
    if env::var("CARGO_CFG_TARGET_OS").as_deref() != Ok("windows") {
        println!("cargo:rustc-link-arg=-Wl,-rpath,{dir}");
        println!("cargo:rustc-link-arg=-Wl,-rpath,$ORIGIN");
    }
}
