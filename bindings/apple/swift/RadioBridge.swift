// RadioBridge — Swift wrapper SKETCH (Phase 3, NOT COMPILED).
//
// Requires Xcode + an Apple device/Mac. Swift-friendly facade over the
// same C core, following the Core Bluetooth delegate model:
//
//   public final class RadioBridge {
//       public init(configuration: Configuration) throws
//       public func scan() -> AsyncStream<Device>
//       public func connect(_ device: Device) async throws -> Connection
//       public func wifiStatus() -> AsyncStream<WifiStatus>
//   }
//
// The async surface is a Phase 3 design decision. The C core stays
// callback-based; Swift wraps it with continuations/streams. See
// ../README.md for the threading and identifier rules.
