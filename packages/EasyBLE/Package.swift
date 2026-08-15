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
    ],
    targets: [
        .target(
            name: "EasyBLE",
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
            swiftSettings: [
                .defaultIsolation(MainActor.self),
            ],
            linkerSettings: [
                .linkedFramework("CoreBluetooth"),
            ]
        ),
        .testTarget(
            name: "EasyBLETests",
            dependencies: ["EasyBLE"],
            swiftSettings: [
                .defaultIsolation(MainActor.self),
            ]
        ),
    ]
)
