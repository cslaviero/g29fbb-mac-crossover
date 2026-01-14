// swift-tools-version: 6.2
// The swift-tools-version declares the minimum version of Swift required to build this package.

import PackageDescription

let package = Package(
    name: "g29ffb",
    products: [
        .executable(name: "g29ffb", targets: ["g29ffb"]),
      
    ],
    targets: [
        .executableTarget(
            name: "g29ffb",
            path: "Sources/g29ffb"
        ),
    ]
)
