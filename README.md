# Web Assembly Nanocontainer Technology for Embedded Devices

- > [CHANGELOG](CHANGELOG.md)

- uses `wasm3` as WA interpreter
- runs multiple [wapps](#wapp-overview) at once as separate threads
- provides isolation through WebAssembly memory model and interaction with outside world is only through VFS drivers
- implements interface to manage the run state of the wapp
    - can start, stop wapp, parameters (e.g. mounted drivers) are defined by JSON config file

## General architecture

> TBD

```
WAPP -> WASI -> VFS -> drivers
```

## Wapp overview

Wapp is RomFs packaged filesystem. It has few requirements:

1. At least 2 files in root directory:

    ```bash
    Some.wapp
    |
    |-> app.wasm
    \-> manifest.json
    ```

    - `app.wasm` is a WebAssembly application to run
    - `manifest.json` is a description of application metadata and requirements
2. Manifest is a JSON file (`manifest.json`), which contains necessary metadata of the application. The template is in `wasm/wapp` folder of this project. Contents:
    - name - application name
    - version - application version
    - chksum - check sum of the wasm file
    - package - package build number (e.g. for different configurations)
    - drivers - required drivers for the application

Rest of the files inside wapp archive is optional and depends on
application. It could be some data or extra configuration files.
