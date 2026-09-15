// swift-tools-version: 6.2

import PackageDescription

let package = Package(
    name: "EasyBLE",
    platforms: [
        .iOS(.v18),
        .macOS(.v14),
    ],
    products: [
        .library(name: "EasyBLE", targets: ["EasyBLE"]),
        .executable(name: "EasyBLEHardwareRunner", targets: ["EasyBLEHardwareRunner"]),
        .executable(name: "EasyBLEAudio", targets: ["EasyBLEAudioCLI"]),
        .executable(name: "VoiceNoteMac", targets: ["VoiceNoteMac"]),
    ],
    targets: [
        .target(
            name: "EasyBLE",
            path: "packages/EasyBLE/Sources/EasyBLE",
            resources: [
                .process("Resources/PrivacyInfo.xcprivacy"),
            ],
            swiftSettings: [
                .defaultIsolation(MainActor.self),
            ],
            linkerSettings: [
                .linkedFramework("CoreBluetooth"),
                .linkedFramework("AccessorySetupKit", .when(platforms: [.iOS])),
            ]
        ),
        .executableTarget(
            name: "EasyBLEHardwareRunner",
            dependencies: ["EasyBLE"],
            path: "packages/EasyBLE/Sources/EasyBLEHardwareRunner",
            swiftSettings: [
                .defaultIsolation(MainActor.self),
            ],
            linkerSettings: [
                .linkedFramework("CoreBluetooth"),
            ]
        ),
        .executableTarget(
            name: "EasyBLEAudioCLI",
            dependencies: ["EasyBLE"],
            path: "packages/EasyBLE/Sources/EasyBLEAudioCLI",
            exclude: ["Info.plist"],
            swiftSettings: [
                .defaultIsolation(MainActor.self),
            ],
            linkerSettings: [
                .linkedFramework("CoreBluetooth"),
                .linkedFramework("AVFoundation"),
            ]
        ),
        .executableTarget(
            name: "VoiceNoteMac",
            dependencies: ["EasyBLE"],
            path: "packages/EasyBLE/Sources/VoiceNoteMac",
            swiftSettings: [
                .defaultIsolation(MainActor.self),
            ],
            linkerSettings: [
                .linkedFramework("CoreBluetooth"),
                .linkedFramework("AVFoundation"),
            ]
        ),
        .testTarget(
            name: "EasyBLETests",
            dependencies: ["EasyBLE"],
            path: "packages/EasyBLE/Tests/EasyBLETests",
            swiftSettings: [
                .defaultIsolation(MainActor.self),
            ]
        ),
    ]
)
