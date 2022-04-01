# Web Assembly Nanocontainer Technology for Embedded Devices

- uses wasm3 as WA interpreter

## Wapp structure

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
