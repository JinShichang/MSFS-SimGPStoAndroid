/********************************************************************
 * MSFS-SimGPStoAndroid 安卓端 - 模拟定位服务
 * 负责：连接电脑端、解析飞行数据、通过系统模拟位置提供者注入位置
 * 参考 ZCShou/GoGoGo 的模拟定位实现（前台服务 + GPS/网络双提供者）
 * 本软件遵循 CC BY-NC-SA 4.0 协议，不得用于商业用途！
 ********************************************************************/

package com.msfs.simconnect.client;

import android.annotation.SuppressLint;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.location.Criteria;
import android.location.Location;
import android.location.LocationManager;
import android.location.provider.ProviderProperties;
import android.os.Binder;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.IBinder;
import android.os.PowerManager;
import android.os.SystemClock;

import androidx.core.app.NotificationCompat;

import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;
import java.net.SocketTimeoutException;
import java.nio.charset.StandardCharsets;

public class MockLocationService extends Service {

    public static final String EXTRA_IP = "ip";
    public static final String EXTRA_PORT = "port";
    public static final String ACTION_STOP_MOCK = "com.msfs.simconnect.client.STOP_MOCK";
    public static final String ACTION_TOGGLE_OVERLAY = "com.msfs.simconnect.client.TOGGLE_OVERLAY";
    public static final String ACTION_OVERLAY_ONLY = "com.msfs.simconnect.client.OVERLAY_ONLY";
    public static final int DEFAULT_PORT = 36666;

    private static final String CHANNEL_ID = "msfs_mock_location";
    private static final String CHANNEL_NAME = "模拟定位服务";
    private static final int NOTIFICATION_ID = 1001;
    private static final long HELLO_INTERVAL_MS = 2000;   // 手机端保活包间隔
    private static final long CONNECT_TIMEOUT_MS = 8000;  // 首包超时
    private static final long LINK_TIMEOUT_MS = 10000;    // 连续收不到任何包判定异常
    private static final long NO_DATA_HOLD_MS = 600;      // 超过该时长无新帧则停止推算、保持原位
    private static final float LATENCY_ALPHA = 0.3f;      // 延迟 EWMA 平滑系数

    /** 一帧飞行数据（电脑端原始单位：高度英尺、速度米/秒） */
    public static class FlightData {
        public double lat;
        public double lon;
        public double altitudeFt;
        public float heading;
        public float pitch;
        public float roll;
        public float groundSpeed;   // m/s
        public float airSpeed;      // m/s
        public boolean valid;

        public String coordsText() {
            String latDir = lat >= 0 ? "N" : "S";
            String lonDir = lon >= 0 ? "E" : "W";
            return String.format("%.6f%s, %.6f%s", Math.abs(lat), latDir, Math.abs(lon), lonDir);
        }
    }

    public interface StatusListener {
        void onConnectionChanged(boolean connected);
        void onMockChanged(boolean mocking);
        void onData(FlightData data);
        void onLatencyChanged(int latencyMs);
        void onOverlayChanged(boolean visible);
        void onError(String message);
    }

    private final MockServiceBinder mBinder = new MockServiceBinder();
    private Handler mMainHandler;
    private StatusListener mListener;

    private LocationManager mLocationManager;
    private SharedPreferences mPrefs;
    private volatile boolean mInjecting = false;
    private Thread mInjectThread;
    private volatile boolean mInjectRunning = false;

    // 平滑补偿参数（手机端可自定义）
    private volatile int mInjectHz = 25;
    private volatile float mHeadingAlpha = 0.3f;
    private volatile float mSpeedAlpha = 0.5f;

    // 预测注入状态：真实帧作为锚点，帧间按航向/地速推算平滑移动
    private volatile FlightData mAnchor;
    private volatile long mAnchorTimeMs;
    private volatile double mCurLat;
    private volatile double mCurLon;
    private volatile float mCurHeading;
    private volatile float mCurSpeed;
    private volatile boolean mSmoothInit = false;
    private volatile long mLastInjectMs;

    private DatagramSocket mUdpSocket;
    private Thread mReceiverThread;
    private Thread mSenderThread;
    private volatile boolean mConnected = false;
    private volatile boolean mDestroyed = false;
    private volatile FlightData mFlightData = new FlightData();
    private volatile long mLastReceiveMs;
    private volatile long mLatencyMs = -1;
    private String mConnectedIp = "";
    private int mConnectedPort = DEFAULT_PORT;

    private PowerManager.WakeLock mWakeLock;
    private StatusOverlay mOverlay;

    @Override
    public void onCreate() {
        super.onCreate();
        mMainHandler = new Handler(getMainLooper());
        mLocationManager = (LocationManager) getSystemService(Context.LOCATION_SERVICE);
        mPrefs = getSharedPreferences("msfssimconnect", MODE_PRIVATE);
        createNotificationChannel();
        startForeground(NOTIFICATION_ID, buildNotification(getString(R.string.conning)));
        acquireWakeLock();
        if (mPrefs.getBoolean("overlay_enabled", true)) {
            showOverlay();
        }
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent == null) return START_NOT_STICKY;
        if (ACTION_STOP_MOCK.equals(intent.getAction())) {
            stopMock();
            return START_NOT_STICKY;
        }
        if (ACTION_TOGGLE_OVERLAY.equals(intent.getAction())) {
            toggleOverlay();
            return START_NOT_STICKY;
        }
        if (ACTION_OVERLAY_ONLY.equals(intent.getAction())) {
            // 仅悬浮窗模式：不连接电脑端，悬浮窗显示"未连接"状态
            return START_NOT_STICKY;
        }
        String ip = intent.getStringExtra(EXTRA_IP);
        int port = intent.getIntExtra(EXTRA_PORT, DEFAULT_PORT);
        if (!mConnected && ip != null && !ip.isEmpty()) {
            connect(ip, port);
        }
        return START_NOT_STICKY;
    }

    @Override
    public IBinder onBind(Intent intent) {
        return mBinder;
    }

    @Override
    public void onDestroy() {
        mDestroyed = true;
        stopMock();
        closeSocket();
        if (mOverlay != null) {
            mOverlay.destroy();
            mOverlay = null;
        }
        if (mWakeLock != null && mWakeLock.isHeld()) {
            mWakeLock.release();
        }
        super.onDestroy();
    }

    // -------------------- 连接电脑端 --------------------

    private void connect(String ip, int port) {
        mConnectedIp = ip;
        mConnectedPort = port;
        new Thread(() -> {
            try {
                DatagramSocket socket = new DatagramSocket();
                socket.setSoTimeout(1000);
                mUdpSocket = socket;
                final long startMs = SystemClock.uptimeMillis();
                mLastReceiveMs = startMs;

                // 接收线程：收数据/心跳，并做超时检测
                mReceiverThread = new Thread(() -> {
                    byte[] buf = new byte[2048];
                    while (!mDestroyed && mUdpSocket != null && !mUdpSocket.isClosed()) {
                        try {
                            DatagramPacket packet = new DatagramPacket(buf, buf.length);
                            mUdpSocket.receive(packet);
                            if (mDestroyed) break;
                            mLastReceiveMs = SystemClock.uptimeMillis();
                            String payload = new String(packet.getData(), 0, packet.getLength(), StandardCharsets.UTF_8).trim();
                            if (payload.startsWith("PONG:")) {
                                handlePong(payload);
                            } else if (payload.startsWith("HEARTBEAT")) {
                                // 纯保活心跳，不参与延迟计算
                            } else if (payload.startsWith("SERVER_FULL")) {
                                handleDisconnect(getString(R.string.server_full));
                                return;
                            } else if (!payload.isEmpty()) {
                                parseLine(payload);
                            }
                            if (!mConnected) {
                                mConnected = true;
                                notifyConnection(true);
                                updateNotification();
                            }
                        } catch (SocketTimeoutException e) {
                            long now = SystemClock.uptimeMillis();
                            if (!mConnected) {
                                if (now - startMs > CONNECT_TIMEOUT_MS) {
                                    handleDisconnect(getString(R.string.conntimeout));
                                    return;
                                }
                            } else if (now - mLastReceiveMs > LINK_TIMEOUT_MS) {
                                handleDisconnect(getString(R.string.link_error));
                                return;
                            }
                        } catch (Exception e) {
                            if (!mDestroyed) {
                                handleDisconnect(getString(R.string.connfail));
                            }
                            return;
                        }
                    }
                }, "MSFS-UDP-Receive");
                mReceiverThread.start();

                // 发送线程：每 2 秒发 HELLO（注册 + 保活 + 延迟测量时间戳）
                mSenderThread = new Thread(() -> {
                    try {
                        InetAddress addr = InetAddress.getByName(ip);
                        while (!mDestroyed && mUdpSocket != null && !mUdpSocket.isClosed()) {
                            byte[] hello = ("HELLO:" + SystemClock.uptimeMillis()).getBytes(StandardCharsets.UTF_8);
                            DatagramPacket out = new DatagramPacket(hello, hello.length, addr, port);
                            mUdpSocket.send(out);
                            Thread.sleep(HELLO_INTERVAL_MS);
                        }
                    } catch (Exception ignored) {
                    }
                }, "MSFS-UDP-Send");
                mSenderThread.start();
            } catch (Exception e) {
                if (!mDestroyed) {
                    handleDisconnect(getString(R.string.connfail));
                }
            }
        }, "MSFS-Socket").start();
    }

    // PONG 延迟测量：电脑端收到 HELLO 后立即回显时间戳，RTT 用 EWMA 平滑（游戏式 ping）
    private void handlePong(String payload) {
        int idx = payload.indexOf(':');
        if (idx < 0) return;
        try {
            long ts = Long.parseLong(payload.substring(idx + 1).trim());
            if (ts > 0) {
                long rtt = SystemClock.uptimeMillis() - ts;
                if (rtt < 0) rtt = 0;
                if (mLatencyMs < 0) {
                    mLatencyMs = rtt;
                } else {
                    mLatencyMs = (long) (LATENCY_ALPHA * rtt + (1 - LATENCY_ALPHA) * mLatencyMs);
                }
                notifyLatency((int) mLatencyMs);
                refreshOverlay();
            }
        } catch (NumberFormatException ignored) {
        }
    }

    private void parseLine(String line) {
        String[] parts = line.split(",");
        if (parts.length != 8) return;
        try {
            FlightData d = new FlightData();
            d.lat = Double.parseDouble(parts[0]);
            d.lon = Double.parseDouble(parts[1]);
            d.altitudeFt = Double.parseDouble(parts[2]);
            d.heading = Float.parseFloat(parts[3]);
            d.pitch = Float.parseFloat(parts[4]);
            d.roll = Float.parseFloat(parts[5]);
            d.groundSpeed = Float.parseFloat(parts[6]);
            d.airSpeed = Float.parseFloat(parts[7]);
            d.valid = true;
            mFlightData = d;
            notifyData(d);
            refreshOverlay();
            if (mInjecting) {
                updateAnchor(d);
                injectFlightData(d);
            }
        } catch (NumberFormatException ignored) {
        }
    }

    private void handleDisconnect(String message) {
        if (mDestroyed) return;
        mConnected = false;
        closeSocket();
        stopMock();
        notifyConnection(false);
        notifyError(message);
        updateNotification();
        if (!(mOverlay != null && mOverlay.isShowing())) {
            stopSelf();
        }
    }

    private void closeSocket() {
        try { if (mUdpSocket != null) mUdpSocket.close(); } catch (Exception ignored) {}
        mUdpSocket = null;
    }

    // -------------------- 模拟定位 --------------------

    public void startMock() {
        if (!mConnected) {
            notifyError(getString(R.string.mock_need_conn));
            return;
        }
        if (mInjecting) return;

        if (!MockLocationHelper.isMockLocationAllowed(this)) {
            notifyError(getString(R.string.mock_permission_denied));
            return;
        }

        mInjecting = true;
        try {
            addTestProviders();
        } catch (SecurityException e) {
            mInjecting = false;
            notifyError(getString(R.string.mock_permission_denied));
            return;
        }
        FlightData d = mFlightData;
        if (d != null && d.valid) {
            updateAnchor(d);
            injectFlightData(d);
        }
        loadCompensationSettings();
        startInjectLoop();
        notifyMock(true);
        refreshOverlay();
        updateNotification();
    }

    public void stopMock() {
        mInjecting = false;
        mInjectRunning = false;
        if (mInjectThread != null) {
            mInjectThread.interrupt();
            mInjectThread = null;
        }
        mAnchor = null;
        mSmoothInit = false;
        mLastInjectMs = 0;
        removeTestProviders();
        notifyMock(false);
        refreshOverlay();
        updateNotification();
    }

    /** 收到一帧真实数据立即注入（作为锚点），帧间由插值循环补平滑 */
    private void injectFlightData(FlightData d) {
        if (d == null || !d.valid || !mInjecting) return;
        double altM = d.altitudeFt * 0.3048;
        setTestLocation(LocationManager.GPS_PROVIDER, d.lat, d.lon, altM, d.heading, d.groundSpeed, 5f, true);
        setTestLocation(LocationManager.NETWORK_PROVIDER, d.lat, d.lon, altM, d.heading, d.groundSpeed, 30f, false);
    }

    /** 更新预测锚点：航向/速度做轻平滑（角度感知），位置直接采用真实帧 */
    private void updateAnchor(FlightData d) {
        mAnchor = d;
        mAnchorTimeMs = SystemClock.uptimeMillis();
        if (!mSmoothInit) {
            mCurLat = d.lat;
            mCurLon = d.lon;
            mCurHeading = d.heading;
            mCurSpeed = d.groundSpeed;
            mSmoothInit = true;
        } else {
            float delta = ((d.heading - mCurHeading + 540f) % 360f) - 180f;
            mCurHeading = mCurHeading + mHeadingAlpha * delta;
            if (mCurHeading < 0) mCurHeading += 360f;
            mCurSpeed = mSpeedAlpha * d.groundSpeed + (1f - mSpeedAlpha) * mCurSpeed;
            mCurLat = d.lat;
            mCurLon = d.lon;
        }
        mLastInjectMs = SystemClock.uptimeMillis();
    }

    /** 读取手机端自定义的补偿参数 */
    private void loadCompensationSettings() {
        int hz = mPrefs.getInt("inject_hz", 25);
        mInjectHz = Math.max(5, Math.min(1000, hz));
        float ha = mPrefs.getFloat("heading_alpha", 0.3f);
        mHeadingAlpha = Math.max(0.05f, Math.min(1f, ha));
        float sa = mPrefs.getFloat("speed_alpha", 0.5f);
        mSpeedAlpha = Math.max(0.05f, Math.min(1f, sa));
    }

    /** 可调频率注入循环：帧间按航向/地速推算平滑位置，按实际经过时间计算 */
    private void startInjectLoop() {
        mInjectRunning = true;
        final long intervalMs = Math.max(1L, 1000L / Math.max(5, mInjectHz));
        mInjectThread = new Thread(() -> {
            while (mInjectRunning && mInjecting && !mDestroyed) {
                tickPrediction();
                try {
                    Thread.sleep(intervalMs);
                } catch (InterruptedException e) {
                    break;
                }
            }
        }, "MSFS-MockInject");
        mInjectThread.start();
    }

    private void tickPrediction() {
        FlightData anchor = mAnchor;
        if (anchor == null || !anchor.valid || !mSmoothInit) return;
        long now = SystemClock.uptimeMillis();

        if (now - mAnchorTimeMs > NO_DATA_HOLD_MS) {
            // 无新帧（如游戏暂停）：保持当前位置，不再推算
            injectPoint(mCurLat, mCurLon, mCurHeading, 0f, anchor.altitudeFt);
            return;
        }

        long fallbackMs = Math.max(1L, 1000L / Math.max(5, mInjectHz));
        long dtMs = mLastInjectMs == 0 ? fallbackMs : Math.min(now - mLastInjectMs, 1000);
        if (dtMs <= 0) dtMs = fallbackMs;
        mLastInjectMs = now;

        double dtSec = dtMs / 1000.0;
        double dist = mCurSpeed * dtSec;
        double headingRad = Math.toRadians(mCurHeading);
        double dNorth = dist * Math.cos(headingRad);
        double dEast = dist * Math.sin(headingRad);
        mCurLat += dNorth / 110574.0;
        mCurLon += dEast / (111320.0 * Math.cos(Math.toRadians(mCurLat)));

        injectPoint(mCurLat, mCurLon, mCurHeading, mCurSpeed, anchor.altitudeFt);
    }

    /** 注入推算点（保持固定频率的平滑流） */
    private void injectPoint(double lat, double lon, float heading, float speed, double altitudeFt) {
        if (!mInjecting) return;
        double altM = altitudeFt * 0.3048;
        setTestLocation(LocationManager.GPS_PROVIDER, lat, lon, altM, heading, speed, 5f, true);
        setTestLocation(LocationManager.NETWORK_PROVIDER, lat, lon, altM, heading, speed, 30f, false);
    }

    private void addTestProviders() {
        addTestProviderGps();
        try {
            addTestProviderNetwork();
        } catch (Exception ignored) {
        }
    }

    @SuppressLint("WrongConstant")
    private void addTestProviderGps() {
        // 参数参考真实 GPS 提供者属性
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            mLocationManager.addTestProvider(LocationManager.GPS_PROVIDER, false, true, false,
                    false, true, true, true,
                    ProviderProperties.POWER_USAGE_HIGH, ProviderProperties.ACCURACY_FINE);
        } else {
            mLocationManager.addTestProvider(LocationManager.GPS_PROVIDER, false, true, false,
                    false, true, true, true,
                    Criteria.POWER_HIGH, Criteria.ACCURACY_FINE);
        }
        if (!mLocationManager.isProviderEnabled(LocationManager.GPS_PROVIDER)) {
            mLocationManager.setTestProviderEnabled(LocationManager.GPS_PROVIDER, true);
        }
    }

    @SuppressLint("WrongConstant")
    private void addTestProviderNetwork() {
        // 参数参考真实 NETWORK 提供者属性
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            mLocationManager.addTestProvider(LocationManager.NETWORK_PROVIDER, true, false,
                    true, true, true, true, true,
                    ProviderProperties.POWER_USAGE_LOW, ProviderProperties.ACCURACY_COARSE);
        } else {
            mLocationManager.addTestProvider(LocationManager.NETWORK_PROVIDER, true, false,
                    true, true, true, true, true,
                    Criteria.POWER_LOW, Criteria.ACCURACY_COARSE);
        }
        if (!mLocationManager.isProviderEnabled(LocationManager.NETWORK_PROVIDER)) {
            mLocationManager.setTestProviderEnabled(LocationManager.NETWORK_PROVIDER, true);
        }
    }

    private void removeTestProviders() {
        try {
            if (mLocationManager.isProviderEnabled(LocationManager.GPS_PROVIDER)) {
                mLocationManager.setTestProviderEnabled(LocationManager.GPS_PROVIDER, false);
            }
            mLocationManager.removeTestProvider(LocationManager.GPS_PROVIDER);
        } catch (Exception ignored) {
        }
        try {
            if (mLocationManager.isProviderEnabled(LocationManager.NETWORK_PROVIDER)) {
                mLocationManager.setTestProviderEnabled(LocationManager.NETWORK_PROVIDER, false);
            }
            mLocationManager.removeTestProvider(LocationManager.NETWORK_PROVIDER);
        } catch (Exception ignored) {
        }
    }

    private void setTestLocation(String provider, double lat, double lon, double altMeters,
                                 float heading, float speed, float accuracy, boolean withSatellites) {
        try {
            Location loc = new Location(provider);
            loc.setAccuracy(accuracy);
            loc.setAltitude(altMeters);
            loc.setBearing(heading);
            loc.setLatitude(lat);
            loc.setLongitude(lon);
            loc.setTime(System.currentTimeMillis());
            loc.setSpeed(speed);
            loc.setElapsedRealtimeNanos(SystemClock.elapsedRealtimeNanos());
            if (withSatellites) {
                Bundle bundle = new Bundle();
                bundle.putInt("satellites", 7);
                loc.setExtras(bundle);
            }
            mLocationManager.setTestProviderLocation(provider, loc);
        } catch (Exception ignored) {
        }
    }

    // -------------------- 悬浮窗 --------------------

    public void showOverlay() {
        if (!MockLocationHelper.isOverlayAllowed(this)) {
            notifyError(getString(R.string.overlay_permission_denied));
            notifyOverlay(false);
            return;
        }
        if (mOverlay == null) {
            mOverlay = new StatusOverlay(this);
            mOverlay.setCallback(visible -> {
                notifyOverlay(visible);
                updateNotification();
            });
        }
        mOverlay.setAlpha(mPrefs.getFloat("overlay_alpha", 0.9f));
        mOverlay.show();
        refreshOverlay();
        notifyOverlay(true);
        updateNotification();
    }

    public void hideOverlay() {
        if (mOverlay != null) {
            mOverlay.hide();
        }
        notifyOverlay(false);
        updateNotification();
    }

    public void toggleOverlay() {
        if (mOverlay != null && mOverlay.isShowing()) {
            hideOverlay();
        } else {
            showOverlay();
        }
    }

    private void refreshOverlay() {
        if (mOverlay != null && mOverlay.isShowing()) {
            mOverlay.update(mConnected, mInjecting, mFlightData, mLatencyMs);
        }
    }

    // -------------------- 通知 --------------------

    private void createNotificationChannel() {
        NotificationChannel channel = new NotificationChannel(CHANNEL_ID, CHANNEL_NAME,
                NotificationManager.IMPORTANCE_LOW);
        NotificationManager manager = (NotificationManager) getSystemService(NOTIFICATION_SERVICE);
        if (manager != null) manager.createNotificationChannel(channel);
    }

    private Notification buildNotification(String content) {
        Intent openIntent = new Intent(this, MainActivity.class);
        PendingIntent openPI = PendingIntent.getActivity(this, 0, openIntent,
                PendingIntent.FLAG_IMMUTABLE);

        NotificationCompat.Builder builder = new NotificationCompat.Builder(this, CHANNEL_ID)
                .setSmallIcon(R.mipmap.ic_launcher)
                .setContentTitle(getString(R.string.app_name))
                .setContentText(content)
                .setContentIntent(openPI)
                .setOngoing(true)
                .setOnlyAlertOnce(true)
                .setPriority(NotificationCompat.PRIORITY_LOW);

        if (mInjecting) {
            Intent stopIntent = new Intent(this, MockLocationService.class).setAction(ACTION_STOP_MOCK);
            PendingIntent stopPI = PendingIntent.getService(this, 1, stopIntent,
                    PendingIntent.FLAG_IMMUTABLE);
            builder.addAction(0, getString(R.string.stop_mock), stopPI);
        }

        Intent overlayIntent = new Intent(this, MockLocationService.class).setAction(ACTION_TOGGLE_OVERLAY);
        PendingIntent overlayPI = PendingIntent.getService(this, 2, overlayIntent,
                PendingIntent.FLAG_IMMUTABLE);
        boolean overlayShowing = mOverlay != null && mOverlay.isShowing();
        builder.addAction(0,
                getString(overlayShowing ? R.string.overlay_hide : R.string.overlay_show),
                overlayPI);
        return builder.build();
    }

    private void updateNotification() {
        String content;
        if (mConnected) {
            content = mConnectedIp + ":" + mConnectedPort
                    + (mInjecting ? " · " + getString(R.string.mock_on) : " · " + getString(R.string.mock_off));
        } else {
            content = getString(R.string.noconnok);
        }
        NotificationManager manager = (NotificationManager) getSystemService(NOTIFICATION_SERVICE);
        if (manager != null) manager.notify(NOTIFICATION_ID, buildNotification(content));
    }

    private void acquireWakeLock() {
        PowerManager pm = (PowerManager) getSystemService(POWER_SERVICE);
        if (pm != null) {
            mWakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "MSFSSimConnect:MockLocation");
            mWakeLock.setReferenceCounted(false);
            mWakeLock.acquire();
        }
    }

    // -------------------- UI 回调 --------------------

    private void notifyConnection(boolean connected) {
        mMainHandler.post(() -> {
            if (mListener != null) mListener.onConnectionChanged(connected);
        });
    }

    private void notifyMock(boolean mocking) {
        mMainHandler.post(() -> {
            if (mListener != null) mListener.onMockChanged(mocking);
        });
    }

    private void notifyData(FlightData data) {
        mMainHandler.post(() -> {
            if (mListener != null) mListener.onData(data);
        });
    }

    private void notifyLatency(int latencyMs) {
        mMainHandler.post(() -> {
            if (mListener != null) mListener.onLatencyChanged(latencyMs);
        });
    }

    private void notifyOverlay(boolean visible) {
        mMainHandler.post(() -> {
            if (mListener != null) mListener.onOverlayChanged(visible);
        });
    }

    private void notifyError(String message) {
        mMainHandler.post(() -> {
            if (mListener != null) mListener.onError(message);
        });
    }

    public class MockServiceBinder extends Binder {
        public void setListener(StatusListener listener) {
            mListener = listener;
        }

        public boolean isConnected() {
            return mConnected;
        }

        public boolean isMocking() {
            return mInjecting;
        }

        public boolean isOverlayVisible() {
            return mOverlay != null && mOverlay.isShowing();
        }

        public FlightData getFlightData() {
            return mFlightData;
        }

        public long getLatencyMs() {
            return mLatencyMs;
        }

        public void startMock() {
            MockLocationService.this.startMock();
        }

        public void stopMock() {
            MockLocationService.this.stopMock();
        }

        public void showOverlay() {
            MockLocationService.this.showOverlay();
        }

        public void hideOverlay() {
            MockLocationService.this.hideOverlay();
        }

        public void setOverlayAlpha(float alpha) {
            if (mOverlay != null) {
                mOverlay.setAlpha(alpha);
            }
        }
    }
}
