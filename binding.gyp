{
  "targets": [
    {
      "target_name": "ifsprojfs",
      "sources": [
        "src/ifsprojfs_bridge.cpp",
        "src/projfs_provider.cpp",
        "src/sync_storage.cpp",
        "src/content_cache.cpp",
        "src/async_bridge.cpp"
      ],
      "include_dirs": [
        "<!@(node -p \"require('node-addon-api').include\")",
        "src"
      ],
      "libraries": [
        "ProjectedFSLib.lib"
      ],
      "defines": [
        "NAPI_CPP_EXCEPTIONS"
      ],
      "cflags!": ["-fno-exceptions"],
      "cflags_cc!": ["-fno-exceptions"],
      "msvs_settings": {
        "VCCLCompilerTool": {
          "ExceptionHandling": 1,
          "AdditionalOptions": ["/std:c++17"]
        }
      },
      "conditions": [
        ["OS=='win'", {
          "defines": ["_WIN32_WINNT=0x0A00"]
        }]
      ]
    }
  ]
}