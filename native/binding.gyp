{
    "targets": [
        {
            "target_name": "streamaudiofilter",
            "sources": [
                "src/addon.cc",
                "src/app_identity.cc",
                "src/engine.cc",
                "src/platform.cc",
                "src/dsp/convert.cc",
                "src/dsp/mixer.cc",
                "src/dsp/resampler.cc",
                "src/dsp/ring_buffer.cc",
                "src/mock/mock_backend.cc",
                "src/win/process_loopback.cc",
                "src/win/session_watcher.cc",
                "src/win/windows_util.cc"
            ],
            "defines": [
                "NAPI_VERSION=8",
                "_WIN32_WINNT=0x0A00",
                "WIN32_LEAN_AND_MEAN",
                "NOMINMAX"
            ],
            "conditions": [
                [
                    "OS=='win'",
                    {
                        "defines": ["UNICODE", "_UNICODE", "WINVER=0x0A00"],
                        "libraries": [
                            "ole32.lib",
                            "oleaut32.lib",
                            "uuid.lib",
                            "version.lib",
                            "advapi32.lib",
                            "mmdevapi.lib",
                            "ksuser.lib"
                        ],
                        "msvs_settings": {
                            "VCCLCompilerTool": {
                                "ExceptionHandling": 1,
                                "AdditionalOptions": [
                                    "/std:c++17",
                                    "/utf-8",
                                    "/permissive-",
                                    "/W4",
                                    "/wd4100"
                                ]
                            }
                        }
                    }
                ],
                [
                    "OS!='win'",
                    {
                        "sources!": [
                            "src/win/process_loopback.cc",
                            "src/win/session_watcher.cc",
                            "src/win/windows_util.cc"
                        ],
                        "cflags_cc": [
                            "-std=c++17",
                            "-finput-charset=UTF-8",
                            "-fexec-charset=UTF-8"
                        ]
                    }
                ]
            ]
        }
    ]
}
