import 'package:flutter/material.dart';
import 'package:mqtt_client/mqtt_client.dart';
import 'package:mqtt_client/mqtt_server_client.dart';
import 'package:flutter_local_notifications/flutter_local_notifications.dart';
import 'package:flutter_svg/flutter_svg.dart';

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
      
      // Configurar listener UMA VEZ antes de inscrever
      client.updates?.listen(_handleMessages, onError: (error) {
        print('❌ Erro no stream MQTT: $error');
      }, cancelOnError: false);
      
      print('👂 Listener MQTT configurado');
      
      // Inscrever nos tópicos DEPOIS de configurar listener
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
      
      // Re-inscrever nos tópicos após reconexão
      client.subscribe('plugalerta/heartbeat', MqttQos.atLeastOnce);
      client.subscribe('plugalerta/alert', MqttQos.atLeastOnce);
      
      print('✅ Reconectado e re-inscrito nos tópicos!');
    } catch (e) {
      print('❌ Falha ao reconectar: $e');
      Future.delayed(const Duration(seconds: 5), () {
        _reconnectMQTT();
      });
    }
  }

  void _handleMessages(List<MqttReceivedMessage<MqttMessage?>>? messages) {
    if (messages == null || messages.isEmpty) return;
    
    final recMess = messages[0].payload as MqttPublishMessage;
    final payload = MqttPublishPayload.bytesToStringAsString(recMess.payload.message);
    final topic = messages[0].topic;

    // Processar imediatamente sem delays
    if (topic == 'plugalerta/heartbeat') {
      _handleHeartbeat(payload);
    } else if (topic == 'plugalerta/alert') {
      _handleAlert(payload);
    }
  }

  void _handleHeartbeat(String payload) {
    // Processar dados ANTES do setState para atualização mais rápida
    String newAcStatus = acStatus;
    Color newAcColor = acColor;
    String newBatteryStatus = batteryStatus;
    Color newBatteryColor = batteryColor;

    // Detectar estado AC
    if (payload.contains('"ac_power":true')) {
      newAcStatus = 'PRESENTE';
      newAcColor = Colors.green;
    } else if (payload.contains('"ac_power":false')) {
      newAcStatus = 'AUSENTE';
      newAcColor = Colors.red;
    }

    // Detectar estado da bateria
    if (payload.contains('"battery_low":false')) {
      newBatteryStatus = 'OK';
      newBatteryColor = Colors.green;
    } else if (payload.contains('"battery_low":true')) {
      newBatteryStatus = 'BAIXA';
      newBatteryColor = Colors.red;
    }

    // Aplicar setState apenas uma vez
    setState(() {
      acStatus = newAcStatus;
      acColor = newAcColor;
      batteryStatus = newBatteryStatus;
      batteryColor = newBatteryColor;
      lastUpdate = 'Atualizado: ${DateTime.now().toString().substring(11, 19)}';
    });
  }

  void _handleAlert(String payload) {
    // Processar dados ANTES do setState
    String newAcStatus = acStatus;
    Color newAcColor = acColor;
    String newBatteryStatus = batteryStatus;
    Color newBatteryColor = batteryColor;

    // Atualizar estado AC do alerta
    if (payload.contains('"ac_power":true')) {
      newAcStatus = 'PRESENTE';
      newAcColor = Colors.green;
    } else if (payload.contains('"ac_power":false')) {
      newAcStatus = 'AUSENTE';
      newAcColor = Colors.red;
    }

    // Atualizar estado da bateria do alerta
    if (payload.contains('"battery_low":false')) {
      newBatteryStatus = 'OK';
      newBatteryColor = Colors.green;
    } else if (payload.contains('"battery_low":true')) {
      newBatteryStatus = 'BAIXA';
      newBatteryColor = Colors.red;
    }

    // Aplicar setState uma única vez
    setState(() {
      acStatus = newAcStatus;
      acColor = newAcColor;
      batteryStatus = newBatteryStatus;
      batteryColor = newBatteryColor;
      lastUpdate = 'Atualizado: ${DateTime.now().toString().substring(11, 19)}';
    });

    // Enviar notificação após atualizar UI
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
          colors: [Color(0xFF1a1a2e), Color(0xFF16213e)],
        ),
      ),
      child: SafeArea(
        child: Scaffold(
          backgroundColor: Colors.transparent,
          body: SingleChildScrollView(
            child: Padding(
              padding: const EdgeInsets.symmetric(horizontal: 24, vertical: 20),
              child: Column(
                children: [
                  _buildHeader(),
                  const SizedBox(height: 40),
                  _buildStatusCard(
                    'Tensão AC',
                    acStatus,
                    acColor,
                    Icons.flash_on,
                  ),
                  const SizedBox(height: 16),
                  _buildStatusCard(
                    'Bateria',
                    batteryStatus,
                    batteryColor,
                    Icons.battery_charging_full,
                  ),
                  const SizedBox(height: 16),
                  _buildStatusCard(
                    'Conexão MQTT',
                    mqttStatus,
                    mqttColor,
                    Icons.cloud_queue,
                  ),
                  const SizedBox(height: 30),
                  _buildLastUpdate(),
                ],
              ),
            ),
          ),
        ),
      ),
    );
  }

  Widget _buildHeader() {
    return Container(
      padding: const EdgeInsets.all(24),
      decoration: BoxDecoration(
        gradient: const LinearGradient(
          begin: Alignment.topLeft,
          end: Alignment.bottomRight,
          colors: [Color(0xFF667eea), Color(0xFF764ba2)],
        ),
        borderRadius: BorderRadius.circular(20),
        boxShadow: [
          BoxShadow(
            color: const Color(0xFF667eea).withOpacity(0.3),
            blurRadius: 20,
            offset: const Offset(0, 10),
          ),
        ],
      ),
      child: Column(
        children: [
          // Logo SVG
          Container(
            height: 80,
            width: 80,
            decoration: BoxDecoration(
              color: Colors.white.withOpacity(0.2),
              shape: BoxShape.circle,
            ),
            child: Padding(
              padding: const EdgeInsets.all(16),
              child: SvgPicture.asset(
                'assets/logo.svg',
                colorFilter: const ColorFilter.mode(
                  Color(0xFFFFD700),
                  BlendMode.srcIn,
                ),
              ),
            ),
          ),
          const SizedBox(height: 16),
          const Text(
            'Plug Alerta',
            style: TextStyle(
              color: Colors.white,
              fontSize: 28,
              fontWeight: FontWeight.bold,
              letterSpacing: 1.2,
            ),
          ),
          const SizedBox(height: 4),
          Text(
            'Monitoramento de Energia',
            style: TextStyle(
              color: Colors.white.withOpacity(0.9),
              fontSize: 14,
              fontWeight: FontWeight.w400,
            ),
          ),
        ],
      ),
    );
  }

  Widget _buildStatusCard(String label, String value, Color color, IconData icon) {
    return Container(
      padding: const EdgeInsets.all(20),
      decoration: BoxDecoration(
        color: const Color(0xFF0f3460),
        borderRadius: BorderRadius.circular(16),
        border: Border.all(
          color: color.withOpacity(0.3),
          width: 2,
        ),
        boxShadow: [
          BoxShadow(
            color: color.withOpacity(0.2),
            blurRadius: 15,
            offset: const Offset(0, 5),
          ),
        ],
      ),
      child: Row(
        children: [
          Container(
            padding: const EdgeInsets.all(12),
            decoration: BoxDecoration(
              color: color.withOpacity(0.2),
              borderRadius: BorderRadius.circular(12),
            ),
            child: Icon(
              icon,
              color: color,
              size: 32,
            ),
          ),
          const SizedBox(width: 16),
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text(
                  label,
                  style: const TextStyle(
                    color: Color(0xFF94a3b8),
                    fontSize: 14,
                    fontWeight: FontWeight.w500,
                  ),
                ),
                const SizedBox(height: 6),
                Text(
                  value,
                  style: TextStyle(
                    color: color,
                    fontSize: 22,
                    fontWeight: FontWeight.bold,
                  ),
                ),
              ],
            ),
          ),
          Container(
            width: 8,
            height: 8,
            decoration: BoxDecoration(
              color: color,
              shape: BoxShape.circle,
              boxShadow: [
                BoxShadow(
                  color: color.withOpacity(0.5),
                  blurRadius: 8,
                  spreadRadius: 2,
                ),
              ],
            ),
          ),
        ],
      ),
    );
  }

  Widget _buildLastUpdate() {
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 20, vertical: 12),
      decoration: BoxDecoration(
        color: const Color(0xFF0f3460).withOpacity(0.5),
        borderRadius: BorderRadius.circular(12),
        border: Border.all(
          color: Colors.white.withOpacity(0.1),
          width: 1,
        ),
      ),
      child: Row(
        mainAxisAlignment: MainAxisAlignment.center,
        children: [
          Icon(
            Icons.access_time,
            color: Colors.white.withOpacity(0.6),
            size: 16,
          ),
          const SizedBox(width: 8),
          Text(
            lastUpdate,
            style: TextStyle(
              color: Colors.white.withOpacity(0.7),
              fontSize: 12,
              fontWeight: FontWeight.w400,
            ),
          ),
        ],
      ),
    );
  }
}
