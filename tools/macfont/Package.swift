// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "macfont",
    platforms: [.macOS(.v13)],
    products: [
        .executable(name: "macfont", targets: ["macfont"]),
    ],
    targets: [
        .executableTarget(
            name: "macfont",
            path: "Sources",
            exclude: ["SelfTestMain.swift"]
        ),
    ]
)
