// swift-tools-version: 6.2

import PackageDescription

let package = Package(
    name: "EasyBLE",
    platforms: [
        .iOS(.v18),
    ],
    products: [
        .library(name: "EasyBLE", targets: ["EasyBLE"]),
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
                .linkedFramework("AccessorySetupKit"),
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
