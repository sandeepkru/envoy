licenses(["notice"])  # Apache 2

load(
    "//bazel:envoy_build_system.bzl",
    "envoy_package",
)

envoy_package()

exports_files([
    "echo_server.json",
    "server.json",
    "server_grpc_json_transcoder.json",
    "server_http2.json",
    "server_http2_upstream.json",
    "server_proxy_proto.json",
    "server_ratelimit.json",
    "server_ssl.json",
    "server_uds.json",
    "server_xfcc.json",
    "tcp_proxy.json",
])

filegroup(
    name = "server_xds_files",
    srcs = [
        "server_xds.bootstrap.yaml",
        "server_xds.cds.yaml",
        "server_xds.eds.yaml",
        "server_xds.json",
        "server_xds.lds.yaml",
        "server_xds.rds.yaml",
    ],
)

filegroup(
    name = "server_config_files",
    srcs = [
        "server.json",
    ],
)
