//! With the `differential` feature, compile the original C utf.c into the
//! test binary under c_-prefixed names so the tests can compare the two.
//!
//! utf.c needs the config.h and options.h of a configured server tree:
//! MOO_BUILD_DIR names that tree (default: the parent directory).

use std::env;
use std::path::PathBuf;

const RENAMED: &[&str] = &[
    "get_utf",
    "get_utf_call",
    "put_utf",
    "utf_byte_index",
    "utf_byte_range",
    "utf_char_index",
    "memo_strlen_utf",
    "clearance_utf",
];

fn main() {
    println!("cargo:rerun-if-env-changed=MOO_BUILD_DIR");
    if env::var_os("CARGO_FEATURE_DIFFERENTIAL").is_none() {
        return;
    }
    let src = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap()).join("..");
    let build = env::var_os("MOO_BUILD_DIR").map_or_else(|| src.clone(), PathBuf::from);
    if !build.join("options.h").exists() {
        panic!("{} is not a configured server tree (no options.h); set MOO_BUILD_DIR", build.display());
    }
    println!("cargo:rerun-if-changed={}", src.join("utf.c").display());

    let mut cc = cc::Build::new();
    cc.file(src.join("utf.c"))
        .include(&build)
        .include(&src)
        .flag("-fexceptions")
        .define("panic", "c_utf_test_panic")
        .warnings(false);
    for name in RENAMED {
        cc.define(name, format!("c_{name}").as_str());
    }
    cc.compile("c_utf");
}
