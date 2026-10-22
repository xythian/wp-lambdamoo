//! Generate Rust bindings for the server's C types and functions from the
//! headers of a configured server tree, so the Rust side always agrees with
//! the C side about Num's size, Var's layout and the like.
//!
//! With the `differential` feature, also compile the original C utf.c into
//! the test binary under c_-prefixed names so the tests can compare the two.
//!
//! Both need the config.h and options.h of a configured server tree:
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
    let manifest = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap());
    let src = manifest.join("..");
    let build = env::var_os("MOO_BUILD_DIR").map_or_else(|| src.clone(), PathBuf::from);
    if !build.join("options.h").exists() {
        panic!("{} is not a configured server tree (no options.h); set MOO_BUILD_DIR", build.display());
    }

    bindings(&manifest, &src, &build);
    if env::var_os("CARGO_FEATURE_DIFFERENTIAL").is_some() {
        differential(&src, &build);
    }
}

fn bindings(manifest: &PathBuf, src: &PathBuf, build: &PathBuf) {
    let wrapper = manifest.join("wrapper.h");
    println!("cargo:rerun-if-changed={}", wrapper.display());
    for header in ["config.h", "options.h"] {
        println!("cargo:rerun-if-changed={}", build.join(header).display());
    }
    for header in ["structures.h", "exceptions.h", "log.h"] {
        println!("cargo:rerun-if-changed={}", src.join(header).display());
    }

    let bindings = bindgen::Builder::default()
        .header(wrapper.to_str().unwrap())
        .clang_arg(format!("-I{}", build.display()))
        .clang_arg(format!("-I{}", src.display()))
        .use_core()
        .allowlist_type("Num|UNum|Objid|Var|var_type")
        .allowlist_function("panic|oklog|errlog|log_perror")
        .allowlist_var("INT_TYPE_BITSIZE")
        .rustified_enum("var_type")
        .derive_debug(false)
        .layout_tests(false)
        .generate()
        .expect("generating bindings for the server headers");
    let out = PathBuf::from(env::var("OUT_DIR").unwrap()).join("bindings.rs");
    bindings.write_to_file(out).expect("writing bindings.rs");
}

fn differential(src: &PathBuf, build: &PathBuf) {
    println!("cargo:rerun-if-changed={}", src.join("utf.c").display());

    let mut cc = cc::Build::new();
    cc.file(src.join("utf.c"))
        .include(build)
        .include(src)
        .flag("-fexceptions")
        .define("panic", "c_utf_test_panic")
        .warnings(false);
    for name in RENAMED {
        cc.define(name, format!("c_{name}").as_str());
    }
    cc.compile("c_utf");
}
