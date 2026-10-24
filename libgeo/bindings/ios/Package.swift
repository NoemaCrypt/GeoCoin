// swift-tools-version:5.9
import PackageDescription

let package = Package(
    name: "LibGeo",
    platforms: [
        .iOS(.v13),
        .macOS(.v12)
    ],
    products: [
        .library(name: "LibGeoSwift", targets: ["LibGeoSwift"])
    ],
    targets: [
        .systemLibrary(
            name: "CLibGeo",
            path: "Sources/CLibGeo"
        ),
        .target(
            name: "LibGeoSwift",
            dependencies: ["CLibGeo"],
            path: "Sources/LibGeoSwift"
        )
    ]
)
