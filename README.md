# MetaShellOS
A fully functional text based OS

## Ready-to-run builds (browser included)

Both binaries already contain the MetaShell Browser, so nothing has to be
compiled or installed to use them:

| File | Platform | Notes |
| --- | --- | --- |
| `main_system.exe` | Windows (x64) | Single file, no DLLs beyond the ones Windows ships with. |
| `main_system-linux` | Linux (x86-64) | Fully static, runs on any distribution. |

Run the Windows build by double-clicking it (or `main_system.exe` in cmd), and
the Linux build with `./main_system-linux`.

## Building from source

The browser is enabled automatically whenever the libcurl headers are present,
so no extra `-D` flag is needed:

```sh
# Linux / macOS
gcc -std=c11 -Wall -Wextra -O2 -o metashell main_system.c $(pkg-config --cflags --libs libcurl)

# Windows (MinGW-w64, static libcurl)
gcc -std=c11 -O2 -o metashell.exe main_system.c -I<curl>/include -L<curl>/lib -static -lcurl -lbcrypt -ladvapi32 -lcrypt32 -lws2_32
```

With `-static` (or the vendored static libcurl above) the resulting binary has
the browser built in and needs no libcurl at run time. Build with `-DNO_CURL`
to leave the browser out on purpose.
