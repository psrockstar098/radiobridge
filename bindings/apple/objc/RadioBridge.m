// RadioBridge — Objective-C wrapper SKETCH (Phase 3, NOT COMPILED).
//
// Requires Xcode + an Apple device/Mac. Shape only:
//
//   @interface RBRadioBridge : NSObject
//   - (instancetype)initWithConfiguration:(RBConfig *)config;
//   - (void)shutdown;
//   - (void)startBLEScanWithOptions:(...) completion:...;
//   - (void)connectToDevice:(NSString *)identifier ...;
//   @end
//
// Implementation notes (see ../README.md):
// - Own a CBCentralManager; forward centralManagerDidUpdateState,
//   didDiscoverPeripheral, didConnectPeripheral, didDisconnectPeripheral
//   and CBPeripheralDelegate service/characteristic/value callbacks
//   into the C callback surface, translating CBPeripheral/CBService/
//   CBCharacteristic into rb_* structs on the spot.
// - Never expose CBPeripheral (or any Core Bluetooth object) in a
//   public header or across the C boundary.
