import 'dart:async';
import 'dart:convert';
import 'package:flutter/material.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';

const String SERVICE_UUID = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
const String CHARACTERISTIC_UUID = "6e400002-b5a3-f393-e0a9-e50e24dcca9e";
const String DEVICE_NAME = "SmartLockBox";

class BluetoothPage extends StatefulWidget {
  const BluetoothPage({super.key});

  @override
  State<BluetoothPage> createState() => _BluetoothPageState();
}

class _BluetoothPageState extends State<BluetoothPage> {
  BluetoothDevice? _device;
  BluetoothCharacteristic? _characteristic;
  bool _connected = false;
  bool _scanning = false;
  bool _isUnlocked = false;
  String _status = 'Not connected';

  StreamSubscription? _scanSubscription;

  @override
  void dispose() {
    _scanSubscription?.cancel();
    _device?.disconnect();
    super.dispose();
  }

  Future<void> _startScan() async {
    setState(() {
      _scanning = true;
      _status = 'Scanning...';
    });

    await FlutterBluePlus.adapterState
        .where((s) => s == BluetoothAdapterState.on)
        .first;

    _scanSubscription?.cancel();

    _scanSubscription = FlutterBluePlus.onScanResults.listen((results) {
      for (ScanResult r in results) {
        debugPrint(
          '>>> Found device: '
          'advName="${r.advertisementData.advName}" '
          'platformName="${r.device.platformName}" '
          'id=${r.device.remoteId}',
        );

        final advName = r.advertisementData.advName;
        final platformName = r.device.platformName;

        if (advName == DEVICE_NAME || platformName == DEVICE_NAME) {
          _scanSubscription?.cancel();
          FlutterBluePlus.stopScan().then((_) => _connectToDevice(r.device));
          return;
        }
      }
    }, onError: (e) => debugPrint('Scan error: $e'));

    await FlutterBluePlus.startScan(
      timeout: const Duration(seconds: 15),
      androidUsesFineLocation: true,
    );

    if (mounted && !_connected) {
      setState(() {
        _scanning = false;
        _status = 'Device not found. Check ESP32 is advertising.';
      });
    }
  }

  Future<void> _connectToDevice(BluetoothDevice device) async {
    setState(() => _status = 'Connecting...');

    try {
      await device.connect(timeout: const Duration(seconds: 10));
      _device = device;

      final services = await device.discoverServices();

      for (BluetoothService service in services) {
        if (service.uuid.toString().toLowerCase() ==
            SERVICE_UUID.toLowerCase()) {
          for (BluetoothCharacteristic c in service.characteristics) {
            if (c.uuid.toString().toLowerCase() ==
                CHARACTERISTIC_UUID.toLowerCase()) {
              _characteristic = c;
              break;
            }
          }
        }
      }

      if (_characteristic != null) {
        if (mounted) {
          setState(() {
            _connected = true;
            _scanning = false;
            _status = 'Connected to SmartLockBox';
          });
        }
      } else {
        if (mounted) {
          setState(() => _status = 'Connected but characteristic not found');
        }
      }
    } catch (e) {
      if (mounted) {
        setState(() {
          _scanning = false;
          _status = 'Connection failed: $e';
        });
      }
    }
  }

  Future<void> _sendCommand(bool unlock) async {
    if (_characteristic == null) return;

    try {
      final List<int> payload = unlock
          ? utf8.encode("true")
          : utf8.encode("false");

      await _characteristic!.write(payload, withoutResponse: false);

      setState(() => _isUnlocked = unlock);

      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(
            content: Text(
              unlock ? '🔓 Unlocked via Bluetooth' : '🔒 Locked via Bluetooth',
            ),
            backgroundColor: unlock ? Colors.green : const Color(0xFFC15F3C),
            duration: const Duration(seconds: 2),
          ),
        );
      }
    } catch (e) {
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(
            content: Text('❌ Failed to send command: $e'),
            backgroundColor: Colors.red,
          ),
        );
      }
    }
  }

  Future<void> _disconnect() async {
    await _device?.disconnect();
    if (mounted) {
      setState(() {
        _connected = false;
        _device = null;
        _characteristic = null;
        _isUnlocked = false;
        _status = 'Disconnected';
      });
    }
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        backgroundColor: const Color(0xFFC15F3C),
        foregroundColor: Colors.white,
        title: const Text(
          'Bluetooth Control',
          style: TextStyle(fontWeight: FontWeight.bold),
        ),
      ),
      body: Padding(
        padding: const EdgeInsets.all(24),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            Card(
              shape: RoundedRectangleBorder(
                borderRadius: BorderRadius.circular(16),
              ),
              elevation: 3,
              child: Padding(
                padding: const EdgeInsets.all(20),
                child: Row(
                  children: [
                    Container(
                      width: 14,
                      height: 14,
                      decoration: BoxDecoration(
                        shape: BoxShape.circle,
                        color: _connected ? Colors.green : Colors.grey,
                      ),
                    ),
                    const SizedBox(width: 12),
                    Expanded(
                      child: Text(
                        _status,
                        style: const TextStyle(
                          fontSize: 15,
                          fontWeight: FontWeight.w500,
                        ),
                      ),
                    ),
                  ],
                ),
              ),
            ),

            const SizedBox(height: 24),

            if (!_connected)
              ElevatedButton.icon(
                onPressed: _scanning ? null : _startScan,
                icon: _scanning
                    ? const SizedBox(
                        width: 18,
                        height: 18,
                        child: CircularProgressIndicator(
                          strokeWidth: 2,
                          color: Colors.white,
                        ),
                      )
                    : const Icon(Icons.bluetooth_searching),
                label: Text(_scanning ? 'Scanning...' : 'Scan for Box'),
                style: ElevatedButton.styleFrom(
                  backgroundColor: const Color(0xFFC15F3C),
                  foregroundColor: Colors.white,
                  padding: const EdgeInsets.symmetric(vertical: 16),
                  shape: RoundedRectangleBorder(
                    borderRadius: BorderRadius.circular(14),
                  ),
                ),
              ),

            if (_connected) ...[
              GestureDetector(
                onTap: () => _sendCommand(true),
                child: Container(
                  height: 70,
                  decoration: BoxDecoration(
                    borderRadius: BorderRadius.circular(20),
                    gradient: LinearGradient(
                      colors: [Colors.green.shade400, Colors.green.shade700],
                    ),
                    boxShadow: [
                      BoxShadow(
                        color: Colors.green.withOpacity(0.4),
                        blurRadius: 12,
                        offset: const Offset(0, 4),
                      ),
                    ],
                  ),
                  child: const Row(
                    mainAxisAlignment: MainAxisAlignment.center,
                    children: [
                      Icon(Icons.lock_open, color: Colors.white, size: 28),
                      SizedBox(width: 12),
                      Text(
                        'Unlock',
                        style: TextStyle(
                          color: Colors.white,
                          fontSize: 18,
                          fontWeight: FontWeight.bold,
                        ),
                      ),
                    ],
                  ),
                ),
              ),

              const SizedBox(height: 16),

              OutlinedButton.icon(
                onPressed: _disconnect,
                icon: const Icon(Icons.bluetooth_disabled),
                label: const Text('Disconnect'),
                style: OutlinedButton.styleFrom(
                  foregroundColor: Colors.grey,
                  padding: const EdgeInsets.symmetric(vertical: 14),
                  shape: RoundedRectangleBorder(
                    borderRadius: BorderRadius.circular(14),
                  ),
                ),
              ),
            ],
          ],
        ),
      ),
    );
  }
}
