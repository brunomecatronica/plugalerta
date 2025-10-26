import 'package:flutter/material.dart';
import 'package:mqtt_client/mqtt_client.dart';
import 'package:mqtt_client/mqtt_server_client.dart';
import 'package:flutter_local_notifications/flutter_local_notifications.dart';

void main() {
  runApp(const PlugAlertaApp());
}

class PlugAlertaApp extends StatelessWidget {
  const PlugAlertaApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'Plug Alerta',
      theme: ThemeData(
        colorScheme: ColorScheme.fromSeed(
          seedColor: const Color(0xFF667eea),
          primary: const Color(0xFF667eea),
        ),
        useMaterial3: true,
      ),
      home: const MainScreen(),
    );
  }
}

class MainScreen extends StatefulWidget {
  const MainScreen({super.key});

  @override
  State<MainScreen> createState() => _MainScreenState();
}

class _MainScreenState extends State<MainScreen> {
  late MqttServerClient client;
  late FlutterLocalNotificationsPlugin notifications;

  String acStatus = 'Aguardando dados...';
  String batteryStatus = 'Aguardando dados...';
  String mqttStatus = 'Conectando...';
  String lastUpdate = 'Aguardando dados...';

  Color acColor = Colors.grey;
  Color batteryColor = Colors.grey;
  Color mqttColor = Colors.grey;

  @override
  void initState() {
    super.initState();
    _initializeNotifications();
    _connectMQTT();
  }

  Future<void> _initializeNotifications() async {
    notifications = FlutterLocalNotificationsPlugin();

    const android = AndroidInitializationSettings('@mipmap/ic_launcher');
    const iOS = DarwinInitializationSettings();
    const initSettings = InitializationSettings(android: android, iOS: iOS);

    await notifications.initialize(initSettings);
    
    // Solicitar permissão de notificações
    print('📱 Solicitando permissão de notificações...');
    try {
      await _requestNotificationPermission();
    } catch (e) {
      print('❌ Erro ao solicitar permissão: $e');
    }
  }

  Future<void> _requestNotificationPermission() async {
    // No Android 13+ precisa solicitar permissão explicitamente
    final androidInfo = await notifications.resolvePlatformSpecificImplementation<
        AndroidFlutterLocalNotificationsPlugin>();
    
    if (androidInfo != null) {
      final granted = await androidInfo.requestNotificationsPermission();
      if (granted ?? false) {
        print('✅ Permissão de notificações concedida');
      } else {
        print('❌ Permissão de notificações negada');
      }
    }
  }

  Future<void> _showNotification(String title, String body) async {
    print('🔔 Chamando _showNotification - Título: $title, Corpo: $body');
    
    const android = AndroidNotificationDetails(
      'plugalerta_channel',
      'Plug Alerta Alertas',
      channelDescription: 'Alertas de mudança de estado da tensão',
      importance: Importance.high,
      priority: Priority.high,
      enableVibration: true,
      playSound: true,
    );
    const iOS = DarwinNotificationDetails();
    const details = NotificationDetails(android: android, iOS: iOS);

    await notifications.show(1, title, body, details);
    print('🔔 Notificação enviada');
  }

  Future<void> _connectMQTT() async {
    client = MqttServerClient.withPort(
      'test.mosquitto.org',
      'PlugAlerta_${DateTime.now().millisecondsSinceEpoch}',
      1883,
    );

    client.logging(on: true);

    client.keepAlivePeriod = 60;
    client.autoReconnect = true;

    final connMess = MqttConnectMessage()
        .withClientIdentifier('PlugAlerta_${DateTime.now().millisecondsSinceEpoch}')
        .withWillTopic('plugalerta/status')
        .withWillMessage('disconnected')
        .startClean()
        .withWillQos(MqttQos.atLeastOnce);

    client.connectionMessage = connMess;

    client.onConnected = () {
      print('✅ Conectado ao broker MQTT');
      setState(() {
        mqttStatus = 'Conectado';
        mqttColor = Colors.green;
      });
    };

    client.onDisconnected = () {
      print('❌ Desconectado do broker MQTT - Tentando reconectar...');
      setState(() {
        mqttStatus = 'Desconectado';
        mqttColor = Colors.red;
      });

      Future.delayed(const Duration(seconds: 3), () {
        _reconnectMQTT();
      });
    };

    try {
      await client.connect();
      client.updates?.listen(_handleMessages);

      client.subscribe('plugalerta/heartbeat', MqttQos.atLeastOnce);
      client.subscribe('plugalerta/alert', MqttQos.atLeastOnce);

      print('✅ Inscrito nos tópicos MQTT (receberá última mensagem retained)');

    } catch (e) {
      print('❌ Erro ao conectar MQTT: $e');
      setState(() {
        mqttStatus = 'Erro';
        mqttColor = Colors.red;
      });

      Future.delayed(const Duration(seconds: 5), () {
        _reconnectMQTT();
      });
    }
  }

  Future<void> _reconnectMQTT() async {
    print('🔄 Tentando reconectar ao MQTT...');
    try {
      await client.connect();
      print('✅ Reconectado com sucesso!');
    } catch (e) {
      print('❌ Falha ao reconectar: $e');
      Future.delayed(const Duration(seconds: 5), () {
        _reconnectMQTT();
      });
    }
  }

  void _handleMessages(List<MqttReceivedMessage<MqttMessage?>>? messages) {
    final recMess = messages![0].payload as MqttPublishMessage;
    final payload = MqttPublishPayload.bytesToStringAsString(recMess.payload.message);
    final topic = messages[0].topic;

    print('📨 Tópico: $topic | Payload: $payload');

    if (topic == 'plugalerta/heartbeat') {
      _handleHeartbeat(payload);
    } else if (topic == 'plugalerta/alert') {
      _handleAlert(payload);
    }
  }

  void _handleHeartbeat(String payload) {
    print('💓 Heartbeat: $payload');

    setState(() {
      if (payload.contains('"ac_power":true')) {
        acStatus = 'PRESENTE';
        acColor = Colors.green;
      } else if (payload.contains('"ac_power":false')) {
        acStatus = 'AUSENTE';
        acColor = Colors.red;
      }

      if (payload.contains('"battery_low":false')) {
        batteryStatus = 'OK';
        batteryColor = Colors.green;
      } else if (payload.contains('"battery_low":true')) {
        batteryStatus = 'BAIXA';
        batteryColor = Colors.red;
      }

      lastUpdate = 'Atualizado: ${DateTime.now().toString().substring(11, 19)}';
    });
  }

  void _handleAlert(String payload) {
    print('🚨 Alerta: $payload');

    const notificationTitle = 'ALERTA TENSÃO';
    String notificationBody = 'Mudança de estado detectada';

    if (payload.contains('AC_LOST')) {
      notificationBody = 'ALERTA TENSÃO DESATIVADA';
    } else if (payload.contains('AC_RESTORED')) {
      notificationBody = 'ALERTA TENSÃO RESTAURADA';
    }

    _showNotification(notificationTitle, notificationBody);
  }

  @override
  void dispose() {
    client.disconnect();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return Container(
      decoration: const BoxDecoration(
        gradient: LinearGradient(
          begin: Alignment.topLeft,
          end: Alignment.bottomRight,
          colors: [Color(0xFF667eea), Color(0xFF764ba2)],
        ),
      ),
      child: SafeArea(
        child: Scaffold(
          backgroundColor: Colors.transparent,
          body: Padding(
            padding: const EdgeInsets.symmetric(horizontal: 20, vertical: 10),
            child: Column(
              children: [
                const Text(
                  'Plug Alerta',
                  style: TextStyle(
                    color: Colors.white,
                    fontSize: 32,
                    fontWeight: FontWeight.bold,
                  ),
                ),
                const SizedBox(height: 20),
                Expanded(
                  child: SingleChildScrollView(
                    child: Column(
                      children: [
                        _buildStatusCard('Status Tensão AC', acStatus, acColor),
                        const SizedBox(height: 15),
                        _buildStatusCard('Status Bateria', batteryStatus, batteryColor),
                        const SizedBox(height: 15),
                        _buildStatusCard('Status MQTT', mqttStatus, mqttColor),
                        const SizedBox(height: 20),
                        Container(
                          padding: const EdgeInsets.all(15),
                          decoration: BoxDecoration(
                            color: Colors.white,
                            borderRadius: BorderRadius.circular(12),
                          ),
                          child: Text(
                            lastUpdate,
                            style: const TextStyle(color: Color(0xFF666666), fontSize: 12),
                            textAlign: TextAlign.center,
                          ),
                        ),
                      ],
                    ),
                  ),
                ),
              ],
            ),
          ),
        ),
      ),
    );
  }

  Widget _buildStatusCard(String label, String value, Color color) {
    return Container(
      padding: const EdgeInsets.all(20),
      decoration: BoxDecoration(
        color: const Color(0xFFF8F9FA),
        borderRadius: BorderRadius.circular(12),
        boxShadow: [
          BoxShadow(
            color: Colors.black.withOpacity(0.1),
            blurRadius: 5,
            offset: const Offset(0, 2),
          ),
        ],
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Text(label, style: const TextStyle(color: Color(0xFF666666), fontSize: 14)),
          const SizedBox(height: 8),
          Text(
            value,
            style: TextStyle(color: color, fontSize: 24, fontWeight: FontWeight.bold),
          ),
        ],
      ),
    );
  }
}
