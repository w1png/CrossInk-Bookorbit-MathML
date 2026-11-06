"""Share identical read-only built-in font tables in the X3/X4 image."""

Import("env")  # noqa: F821  # injected by PlatformIO/SCons


def merge_builtin_font_constants(node_env, node):
    font_env = node_env.Clone()
    # Only this translation unit owns the arrays. The renderer uses their
    # contents and keeps each font's separate EpdFontData as its identity.
    font_env.Append(CXXFLAGS=["-fmerge-all-constants"])
    return font_env.Object(node)


if env.BoardConfig().get("build.mcu") == "esp32c3":  # noqa: F821
    env.AddBuildMiddleware(merge_builtin_font_constants, "*src/BuiltinFonts.cpp")  # noqa: F821
