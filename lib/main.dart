import 'dart:async';
import 'dart:convert';
import 'dart:math';
import 'package:flutter/material.dart';
import 'package:http/http.dart' as http;
import 'bluetooth_page.dart';

import 'package:firebase_core/firebase_core.dart';
import 'package:firebase_messaging/firebase_messaging.dart';

import 'package:flutter_local_notifications/flutter_local_notifications.dart';

@pragma('vm:entry-point')
Future<void> _firebaseMessagingBackgroundHandler(RemoteMessage message) async {
  await Firebase.initializeApp();
  print("Background message received: ${message.messageId}");
}

void main() async {
  WidgetsFlutterBinding.ensureInitialized();
  await Firebase.initializeApp();
  FirebaseMessaging.onBackgroundMessage(_firebaseMessagingBackgroundHandler);
  runApp(const SmartDeliveryApp());
}

const String BASE_URL =
    'https://server-smart-delivery-systme-production.up.railway.app';
const Duration REFRESH_INTERVAL = Duration(seconds: 5);

enum LockAction { none, unlocked, locked }

final Map<String, LockAction> imageActions = {};

class SmartDeliveryApp extends StatelessWidget {
  const SmartDeliveryApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'Smart Delivery Box',
      debugShowCheckedModeBanner: false,
      theme: ThemeData(
        colorScheme: ColorScheme.fromSeed(seedColor: const Color(0xFFC15F3C)),
        useMaterial3: true,
        fontFamily: 'Montserrat',
      ),
      home: const HomePage(),
    );
  }
}

class DeliveryImage {
  final String filename;
  final String timestamp;
  final String url;

  DeliveryImage({
    required this.filename,
    required this.timestamp,
    required this.url,
  });

  factory DeliveryImage.fromJson(Map<String, dynamic> json) {
    return DeliveryImage(
      filename: json['filename'],
      timestamp: json['timestamp'],
      url: json['url'],
    );
  }

  String get displayTime {
    try {
      final parts = timestamp.split('T');
      final dateParts = parts[0].split('-');
      final timeParts = parts[1].split('-');
      final months = [
        '',
        'Jan',
        'Feb',
        'Mar',
        'Apr',
        'May',
        'Jun',
        'Jul',
        'Aug',
        'Sep',
        'Oct',
        'Nov',
        'Dec',
      ];
      final month = months[int.parse(dateParts[1])];
      final day = dateParts[2];
      final year = dateParts[0];
      final time = '${timeParts[0]}:${timeParts[1]}:${timeParts[2]}';
      return '$month $day, $year  $time';
    } catch (_) {
      return timestamp;
    }
  }
}

class HomePage extends StatefulWidget {
  const HomePage({super.key});

  @override
  State<HomePage> createState() => _HomePageState();
}

class _HomePageState extends State<HomePage> {
  List<DeliveryImage> _images = [];
  bool _loading = true;
  String? _error;
  Timer? _timer;
  String? _lastFilename;
  int _selectedIndex = 0;

  bool _isUnlocked = false;
  bool _lockLoading = false;
  DateTime? _lastImageReceivedAt;

  @override
  void initState() {
    super.initState();
    _fetchImages();
    _initNotifications();
    _timer = Timer.periodic(REFRESH_INTERVAL, (_) {
      _fetchImages();
    });
  }

  Future<void> _initNotifications() async {
    FirebaseMessaging messaging = FirebaseMessaging.instance;

    await messaging.requestPermission();

    messaging.onTokenRefresh.listen((fcmToken) {
      print("FCM Token (refresh): $fcmToken");
      _registerToken(fcmToken);
    });

    String? apnsToken;
    for (int i = 0; i < 5; i++) {
      apnsToken = await messaging.getAPNSToken();
      print("APNs attempt $i: $apnsToken");
      if (apnsToken != null) break;
      await Future.delayed(const Duration(seconds: 2));
    }

    if (apnsToken != null) {
      final token = await messaging.getToken();
      print("FCM Token: $token");
      if (token != null) {
        await _registerToken(token);
      }
    } else {
      print("APNs token failed after retries");
      final token = await messaging.getToken();
      print("FCM Token (Android): $token");
      if (token != null) {
        await _registerToken(token);
      }
    }

    FirebaseMessaging.onMessage.listen((RemoteMessage message) {
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(
            content: Text(message.notification?.body ?? ' New delivery!'),
            backgroundColor: const Color(0xFFC15F3C),
            duration: const Duration(seconds: 4),
          ),
        );
      }
    });

    FirebaseMessaging.onMessageOpenedApp.listen((RemoteMessage message) {
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(
            content: Text(message.notification?.body ?? ' New delivery!'),
            backgroundColor: const Color(0xFFC15F3C),
            duration: const Duration(seconds: 4),
          ),
        );
      }
    });
  }

  Future<void> _registerToken(String token) async {
    try {
      await http.post(
        Uri.parse('$BASE_URL/register-token'),
        headers: {'Content-Type': 'application/json'},
        body: jsonEncode({'token': token}),
      );
      print("Token registered with server");
    } catch (_) {}
  }

  @override
  void dispose() {
    _timer?.cancel();
    super.dispose();
  }

  Future<void> _fetchImages() async {
    try {
      final response = await http
          .get(Uri.parse('$BASE_URL/images'))
          .timeout(const Duration(seconds: 10));

      if (response.statusCode == 200) {
        final List<dynamic> data = jsonDecode(response.body);
        final images = data.map((e) => DeliveryImage.fromJson(e)).toList();

        final newLatest = images.isNotEmpty ? images.first.filename : null;
        final isNew =
            newLatest != null &&
            newLatest != _lastFilename &&
            _lastFilename != null;

        setState(() {
          _images = images;
          _loading = false;
          _error = null;
          _lastFilename = newLatest;
        });

        if (isNew && mounted) {
          setState(() {
            _isUnlocked = false;
            final latestFilename = images.isNotEmpty
                ? images.first.filename
                : null;
            if (latestFilename != null) {
              imageActions[latestFilename] = LockAction.locked;
            }
            _lastImageReceivedAt = DateTime.now();
          });

          ScaffoldMessenger.of(context).showSnackBar(
            const SnackBar(
              content: Text('New delivery image received!'),
              duration: Duration(seconds: 3),
              backgroundColor: Color(0xFFC15F3C),
            ),
          );
        }
      } else {
        setState(() {
          _error = 'Server error: ${response.statusCode}';
          _loading = false;
        });
      }
    } catch (e) {
      setState(() {
        _error = 'Cannot reach server. Check your connection.';
        _loading = false;
      });
    }
  }

  Future<void> _toggleLock() async {
    if (_lockLoading || _images.isEmpty) return;

    setState(() => _lockLoading = true);

    try {
      final response = await http
          .post(Uri.parse('$BASE_URL/unlock'))
          .timeout(const Duration(seconds: 10));

      if (response.statusCode == 200) {
        setState(() {
          _isUnlocked = true;
          final latest = _images.first;
          imageActions[latest.filename] = LockAction.unlocked;
        });

        if (mounted) {
          ScaffoldMessenger.of(context).showSnackBar(
            const SnackBar(
              content: Text('Unlock command sent!'),
              duration: Duration(seconds: 2),
              backgroundColor: Colors.green,
            ),
          );
        }
      } else {
        if (mounted) {
          ScaffoldMessenger.of(context).showSnackBar(
            const SnackBar(
              content: Text('Failed to send command. Try again.'),
              backgroundColor: Colors.red,
            ),
          );
        }
      }
    } catch (e) {
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          const SnackBar(
            content: Text('Cannot reach server. Check your connection.'),
            backgroundColor: Colors.red,
          ),
        );
      }
    } finally {
      setState(() => _lockLoading = false);
    }
  }

  void _onNavBarTapped(int index) {
    setState(() => _selectedIndex = index);
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: PreferredSize(
        preferredSize: const Size.fromHeight(70),
        child: AppBar(
          elevation: 0,
          backgroundColor: Colors.transparent,
          flexibleSpace: Container(
            decoration: const BoxDecoration(
              gradient: LinearGradient(
                colors: [Color(0xFFC15F3C), Color(0xFFB1ADA1)],
                begin: Alignment.topLeft,
                end: Alignment.bottomRight,
              ),
            ),
          ),
          title: Row(
            children: [
              const Icon(Icons.inventory_2_outlined, color: Colors.white),
              IconButton(
                icon: const Icon(Icons.bluetooth, color: Colors.white),
                onPressed: () => Navigator.push(
                  context,
                  MaterialPageRoute(builder: (_) => const BluetoothPage()),
                ),
              ),
              const SizedBox(width: 6),
              const Expanded(
                child: Text(
                  'Smart Delivery Box',
                  maxLines: 1,
                  overflow: TextOverflow.ellipsis,
                  style: TextStyle(
                    color: Colors.white,
                    fontWeight: FontWeight.bold,
                    fontSize: 18,
                    letterSpacing: 0.8,
                  ),
                ),
              ),
            ],
          ),
          actions: [
            Padding(
              padding: const EdgeInsets.only(right: 18),
              child: Row(
                children: [
                  Container(
                    width: 10,
                    height: 10,
                    decoration: BoxDecoration(
                      color: _error == null
                          ? Colors.greenAccent
                          : Colors.redAccent,
                      shape: BoxShape.circle,
                      boxShadow: [
                        BoxShadow(
                          color:
                              (_error == null
                                      ? Colors.greenAccent
                                      : Colors.redAccent)
                                  .withOpacity(0.6),
                          blurRadius: 8,
                          spreadRadius: 1,
                        ),
                      ],
                    ),
                  ),
                  const SizedBox(width: 8),
                  Text(
                    _error == null ? 'Live' : 'Offline',
                    style: const TextStyle(fontSize: 13, color: Colors.white),
                  ),
                ],
              ),
            ),
          ],
        ),
      ),
      body: Container(
        decoration: const BoxDecoration(
          gradient: LinearGradient(
            colors: [Color(0xFFF4F3EE), Color(0xFFFFFFFF)],
            begin: Alignment.topCenter,
            end: Alignment.bottomCenter,
          ),
        ),
        child: _loading
            ? const Center(child: CircularProgressIndicator())
            : _error != null
            ? _buildError()
            : _images.isEmpty
            ? _buildEmpty()
            : (_selectedIndex == 0 ? _buildLatestTab() : _buildHistoryTab()),
      ),
      bottomNavigationBar: BottomNavigationBar(
        currentIndex: _selectedIndex,
        onTap: _onNavBarTapped,
        backgroundColor: Colors.white,
        selectedItemColor: const Color(0xFFC15F3C),
        unselectedItemColor: Colors.grey,
        items: const [
          BottomNavigationBarItem(
            icon: Icon(Icons.photo_camera_front),
            label: 'Latest',
          ),
          BottomNavigationBarItem(icon: Icon(Icons.history), label: 'History'),
        ],
      ),
    );
  }

  Widget _buildError() {
    return Center(
      child: Column(
        mainAxisAlignment: MainAxisAlignment.center,
        children: [
          const Icon(Icons.wifi_off, size: 64, color: Colors.grey),
          const SizedBox(height: 16),
          Text(_error!, style: const TextStyle(color: Colors.grey)),
          const SizedBox(height: 16),
          ElevatedButton(onPressed: _fetchImages, child: const Text('Retry')),
        ],
      ),
    );
  }

  Widget _buildEmpty() {
    return const Center(
      child: Column(
        mainAxisAlignment: MainAxisAlignment.center,
        children: [
          Icon(Icons.inbox_outlined, size: 64, color: Colors.grey),
          SizedBox(height: 16),
          Text(
            'No deliveries yet',
            style: TextStyle(fontSize: 18, color: Colors.grey),
          ),
          SizedBox(height: 8),
          Text(
            'Images will appear here when someone rings the bell.',
            style: TextStyle(color: Colors.grey),
            textAlign: TextAlign.center,
          ),
        ],
      ),
    );
  }

  Widget _buildLatestTab() {
    final latest = _images.first;
    final action = imageActions[latest.filename] ?? LockAction.none;
    final effectiveIsUnlocked = action == LockAction.none
        ? _isUnlocked
        : (action == LockAction.unlocked);

    return RefreshIndicator(
      onRefresh: _fetchImages,
      child: ListView(
        padding: const EdgeInsets.all(16),
        children: [
          const _SectionLabel(label: 'Latest Delivery'),
          const SizedBox(height: 8),
          _LatestImageCard(image: latest),
          const SizedBox(height: 20),
          _LockUnlockButton(
            isUnlocked: effectiveIsUnlocked,
            isLoading: _lockLoading,
            onTap: _toggleLock,
          ),
        ],
      ),
    );
  }

  Widget _buildHistoryTab() {
    final latest = _images.first;
    return RefreshIndicator(
      onRefresh: _fetchImages,
      child: ListView(
        padding: const EdgeInsets.all(16),
        children: [
          _SectionLabel(label: 'All Deliveries (${_images.length})'),
          const SizedBox(height: 8),
          ..._images
              .map(
                (img) => _ImageListTile(
                  image: img,
                  isLatest: img.filename == latest.filename,
                  action: imageActions[img.filename] ?? LockAction.none,
                ),
              )
              .toList(),
        ],
      ),
    );
  }
}

class _LockUnlockButton extends StatelessWidget {
  final bool isUnlocked;
  final bool isLoading;
  final VoidCallback onTap;

  const _LockUnlockButton({
    required this.isUnlocked,
    required this.isLoading,
    required this.onTap,
  });

  @override
  Widget build(BuildContext context) {
    return GestureDetector(
      onTap: isLoading ? null : onTap,
      child: AnimatedContainer(
        duration: const Duration(milliseconds: 300),
        curve: Curves.easeInOut,
        height: 64,
        decoration: BoxDecoration(
          borderRadius: BorderRadius.circular(20),
          gradient: LinearGradient(
            colors: isUnlocked
                ? [Colors.green.shade400, Colors.green.shade700]
                : [const Color(0xFFC15F3C), const Color(0xFF8B3A1E)],
            begin: Alignment.topLeft,
            end: Alignment.bottomRight,
          ),
          boxShadow: [
            BoxShadow(
              color: (isUnlocked ? Colors.green : const Color(0xFFC15F3C))
                  .withOpacity(0.4),
              blurRadius: 12,
              offset: const Offset(0, 4),
            ),
          ],
        ),
        child: isLoading
            ? const Center(
                child: CircularProgressIndicator(color: Colors.white),
              )
            : Row(
                mainAxisAlignment: MainAxisAlignment.center,
                children: [
                  Icon(
                    isUnlocked ? Icons.lock_open : Icons.lock,
                    color: Colors.white,
                    size: 26,
                  ),
                  const SizedBox(width: 12),
                  Text(
                    isUnlocked ? 'Box Unlocked' : 'Tap to Unlock Box',
                    style: const TextStyle(
                      color: Colors.white,
                      fontSize: 17,
                      fontWeight: FontWeight.bold,
                      letterSpacing: 0.5,
                    ),
                  ),
                ],
              ),
      ),
    );
  }
}

class _LatestImageCard extends StatelessWidget {
  final DeliveryImage image;

  const _LatestImageCard({required this.image});

  @override
  Widget build(BuildContext context) {
    return GestureDetector(
      onTap: () => Navigator.push(
        context,
        MaterialPageRoute(builder: (_) => ImageViewPage(image: image)),
      ),
      child: Card(
        elevation: 8,
        shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(22)),
        clipBehavior: Clip.antiAlias,
        shadowColor: const Color(0xFFC15F3C).withOpacity(0.18),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            AspectRatio(
              aspectRatio: 16 / 9,
              child: Hero(
                tag: image.url,
                child: Transform.rotate(
                  angle: pi,
                  child: Image.network(
                    image.url,
                    fit: BoxFit.cover,
                    loadingBuilder: (_, child, progress) {
                      if (progress == null) return child;
                      return const Center(child: CircularProgressIndicator());
                    },
                    errorBuilder: (_, __, ___) => Container(
                      color: Colors.grey[200],
                      child: const Center(
                        child: Icon(
                          Icons.broken_image,
                          size: 48,
                          color: Colors.grey,
                        ),
                      ),
                    ),
                  ),
                ),
              ),
            ),
            Padding(
              padding: const EdgeInsets.all(16),
              child: Row(
                children: [
                  const Icon(
                    Icons.access_time,
                    size: 18,
                    color: Color(0xFFC15F3C),
                  ),
                  const SizedBox(width: 8),
                  Text(
                    image.displayTime,
                    style: const TextStyle(
                      fontSize: 15,
                      color: Color(0xFFC15F3C),
                      fontWeight: FontWeight.w600,
                      letterSpacing: 0.5,
                    ),
                  ),
                ],
              ),
            ),
          ],
        ),
      ),
    );
  }
}

class _ImageListTile extends StatelessWidget {
  final DeliveryImage image;
  final bool isLatest;
  final LockAction action;

  const _ImageListTile({
    required this.image,
    required this.isLatest,
    required this.action,
  });

  Map<String, dynamic> get _actionInfo {
    switch (action) {
      case LockAction.unlocked:
        return {
          'icon': Icons.lock_open,
          'label': 'Unlocked',
          'color': Colors.green,
        };
      case LockAction.locked:
        return {
          'icon': Icons.lock,
          'label': 'Locked',
          'color': const Color(0xFFC15F3C),
        };
      case LockAction.none:
        return {
          'icon': Icons.help_outline,
          'label': 'No action taken',
          'color': Colors.grey,
        };
    }
  }

  void _showActionDialog(BuildContext context) {
    final info = _actionInfo;
    showDialog(
      context: context,
      builder: (_) => Dialog(
        shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(20)),
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            ClipRRect(
              borderRadius: const BorderRadius.vertical(
                top: Radius.circular(20),
              ),
              child: Transform.rotate(
                angle: pi,
                child: Image.network(
                  image.url,
                  height: 200,
                  width: double.infinity,
                  fit: BoxFit.cover,
                  errorBuilder: (_, __, ___) => Container(
                    height: 200,
                    color: Colors.grey[200],
                    child: const Center(
                      child: Icon(Icons.broken_image, color: Colors.grey),
                    ),
                  ),
                ),
              ),
            ),
            Padding(
              padding: const EdgeInsets.all(20),
              child: Column(
                children: [
                  Row(
                    children: [
                      const Icon(
                        Icons.access_time,
                        size: 16,
                        color: Color(0xFFC15F3C),
                      ),
                      const SizedBox(width: 6),
                      Text(
                        image.displayTime,
                        style: const TextStyle(
                          fontSize: 14,
                          color: Color(0xFFC15F3C),
                          fontWeight: FontWeight.w600,
                        ),
                      ),
                    ],
                  ),
                  const SizedBox(height: 16),
                  Container(
                    width: double.infinity,
                    padding: const EdgeInsets.symmetric(
                      vertical: 14,
                      horizontal: 16,
                    ),
                    decoration: BoxDecoration(
                      color: (info['color'] as Color).withOpacity(0.1),
                      borderRadius: BorderRadius.circular(12),
                      border: Border.all(
                        color: (info['color'] as Color).withOpacity(0.3),
                      ),
                    ),
                    child: Row(
                      children: [
                        Icon(
                          info['icon'] as IconData,
                          color: info['color'] as Color,
                          size: 22,
                        ),
                        const SizedBox(width: 10),
                        Column(
                          crossAxisAlignment: CrossAxisAlignment.start,
                          children: [
                            const Text(
                              'Action Taken',
                              style: TextStyle(
                                fontSize: 11,
                                color: Colors.grey,
                              ),
                            ),
                            Text(
                              info['label'] as String,
                              style: TextStyle(
                                fontSize: 16,
                                fontWeight: FontWeight.bold,
                                color: info['color'] as Color,
                              ),
                            ),
                          ],
                        ),
                      ],
                    ),
                  ),
                  const SizedBox(height: 16),
                  SizedBox(
                    width: double.infinity,
                    child: TextButton(
                      onPressed: () => Navigator.pop(context),
                      child: const Text('Close'),
                    ),
                  ),
                ],
              ),
            ),
          ],
        ),
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    final info = _actionInfo;
    return AnimatedContainer(
      duration: const Duration(milliseconds: 350),
      curve: Curves.easeInOut,
      child: Card(
        margin: const EdgeInsets.only(bottom: 10),
        shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(16)),
        elevation: isLatest ? 7 : 2,
        shadowColor: isLatest
            ? const Color(0xFFC15F3C).withOpacity(0.18)
            : Colors.black12,
        child: ListTile(
          onTap: () => _showActionDialog(context),
          leading: ClipRRect(
            borderRadius: BorderRadius.circular(10),
            child: Transform.rotate(
              angle: pi,
              child: Image.network(
                image.url,
                width: 56,
                height: 56,
                fit: BoxFit.cover,
                errorBuilder: (_, __, ___) => Container(
                  width: 56,
                  height: 56,
                  color: Colors.grey[200],
                  child: const Icon(Icons.broken_image, color: Colors.grey),
                ),
              ),
            ),
          ),
          title: Text(
            image.displayTime,
            style: TextStyle(
              fontSize: 15,
              fontWeight: FontWeight.w600,
              color: isLatest ? const Color(0xFFC15F3C) : Colors.black87,
              letterSpacing: 0.2,
            ),
          ),
          subtitle: Row(
            children: [
              Icon(
                info['icon'] as IconData,
                size: 13,
                color: info['color'] as Color,
              ),
              const SizedBox(width: 4),
              Text(
                info['label'] as String,
                style: TextStyle(
                  fontSize: 12,
                  color: info['color'] as Color,
                  fontWeight: FontWeight.w500,
                ),
              ),
            ],
          ),
          trailing: const Icon(Icons.chevron_right, color: Colors.grey),
        ),
      ),
    );
  }
}

class ImageViewPage extends StatelessWidget {
  final DeliveryImage image;

  const ImageViewPage({super.key, required this.image});

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      backgroundColor: Colors.black,
      appBar: AppBar(
        backgroundColor: Colors.transparent,
        elevation: 0,
        foregroundColor: Colors.white,
        title: Text(
          image.displayTime,
          style: const TextStyle(
            fontSize: 15,
            fontWeight: FontWeight.w600,
            letterSpacing: 0.5,
          ),
        ),
      ),
      body: Center(
        child: InteractiveViewer(
          child: Hero(
            tag: image.url,
            child: Transform.rotate(
              angle: pi,
              child: Image.network(
                image.url,
                fit: BoxFit.contain,
                loadingBuilder: (_, child, progress) {
                  if (progress == null) return child;
                  return const Center(
                    child: CircularProgressIndicator(color: Colors.white),
                  );
                },
                errorBuilder: (_, __, ___) => const Center(
                  child: Icon(Icons.broken_image, size: 64, color: Colors.grey),
                ),
              ),
            ),
          ),
        ),
      ),
    );
  }
}

class _SectionLabel extends StatelessWidget {
  final String label;

  const _SectionLabel({required this.label});

  @override
  Widget build(BuildContext context) {
    return Text(
      label,
      style: const TextStyle(
        fontSize: 16,
        fontWeight: FontWeight.bold,
        color: Colors.black87,
      ),
    );
  }
}
