// Leak reports to ignore in sanitizer builds: caches that libraries keep for
// the life of the process on purpose (fontconfig, Pango, GLib type system).
// Linked into executables only: the runtime looks this function up by name.

extern "C" const char* __lsan_default_suppressions()
{
    return "leak:libfontconfig.so\n"
           "leak:libpangoft2-1.0.so\n"
           "leak:libpango-1.0.so\n"
           "leak:libpangocairo-1.0.so\n"
           "leak:libglib-2.0.so\n"
           "leak:libgobject-2.0.so\n"
           "leak:libharfbuzz.so\n"
           "leak:libcairo.so\n";
}
