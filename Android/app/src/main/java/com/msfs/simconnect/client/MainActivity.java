/********************************************************************
 * MSFS-SimGPStoAndroid 安卓端 - 主界面
 * 连接电脑端 + 模拟定位开关 + 状态悬浮窗开关 + 状态显示
 * 本软件遵循 CC BY-NC-SA 4.0 协议，不得用于商业用途！
 ********************************************************************/

package com.msfs.simconnect.client;

import android.app.AlertDialog;
import android.content.ComponentName;
import android.content.Intent;
import android.content.ServiceConnection;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Bundle;
import android.os.IBinder;
import android.os.PowerManager;
import android.provider.Settings;
import android.view.View;
import android.widget.Button;
import android.widget.CompoundButton;
import android.widget.EditText;
import android.widget.ProgressBar;
import android.widget.Switch;
import android.widget.TextView;
import android.widget.Toast;

import androidx.annotation.Nullable;
import androidx.appcompat.app.AppCompatActivity;
import androidx.core.content.ContextCompat;

import com.yxing.ScanCodeConfig;
import com.yxing.def.ScanMode;
import com.yxing.def.ScanStyle;

public class MainActivity extends AppCompatActivity implements MockLocationService.StatusListener {

    private static final int REQ_OVERLAY_PERMISSION = 1001;
    private static final int REQ_IGNORE_BATTERY = 1002;
    private static final int ALBUM_QUEST_CODE = 0;
    private boolean mPendingScan = false;

    private EditText ipInput;
    private EditText portInput;
    private EditText injectHzInput;
    private EditText headingAlphaInput;
    private EditText speedAlphaInput;
    private EditText overlayAlphaInput;
    private Button connectBtn;
    private Button scanBtn;
    private ProgressBar loadingAnim;
    private TextView statusText;
    private Switch mockSwitch;
    private Switch overlaySwitch;

    private SharedPreferences mPrefs;
    private MockLocationService.MockServiceBinder mBinder;
    private boolean mBound = false;
    private boolean mConnected = false;
    private boolean mConnecting = false;
    private String mConnectedIp = "";
    private int mConnectedPort = MockLocationService.DEFAULT_PORT;
    private CompoundButton.OnCheckedChangeListener mMockListener;
    private CompoundButton.OnCheckedChangeListener mOverlayListener;

    private final ServiceConnection mConnection = new ServiceConnection() {
        @Override
        public void onServiceConnected(ComponentName name, IBinder service) {
            mBinder = (MockLocationService.MockServiceBinder) service;
            mBinder.setListener(MainActivity.this);
            mBound = true;
            syncFromService();
        }

        @Override
        public void onServiceDisconnected(ComponentName name) {
            mBinder = null;
            mBound = false;
            mConnected = false;
            resetToDisconnected();
        }
    };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        mPrefs = getSharedPreferences("msfssimconnect", MODE_PRIVATE);

        ipInput = findViewById(R.id.ipInput);
        portInput = findViewById(R.id.portInput);
        injectHzInput = findViewById(R.id.injectHzInput);
        headingAlphaInput = findViewById(R.id.headingAlphaInput);
        speedAlphaInput = findViewById(R.id.speedAlphaInput);
        overlayAlphaInput = findViewById(R.id.overlayAlphaInput);
        connectBtn = findViewById(R.id.connectBtn);
        scanBtn = findViewById(R.id.scanBtn);
        loadingAnim = findViewById(R.id.loadingAnim);
        statusText = findViewById(R.id.statusText);
        mockSwitch = findViewById(R.id.mockSwitch);
        overlaySwitch = findViewById(R.id.overlaySwitch);

        ipInput.setText(mPrefs.getString("ip", ""));
        portInput.setText(mPrefs.getString("port", String.valueOf(MockLocationService.DEFAULT_PORT)));
        injectHzInput.setText(String.valueOf(mPrefs.getInt("inject_hz", 25)));
        headingAlphaInput.setText(String.valueOf(mPrefs.getFloat("heading_alpha", 0.3f)));
        speedAlphaInput.setText(String.valueOf(mPrefs.getFloat("speed_alpha", 0.5f)));
        overlayAlphaInput.setText(String.valueOf(mPrefs.getFloat("overlay_alpha", 0.9f)));
        overlayAlphaInput.setOnFocusChangeListener((v, hasFocus) -> {
            if (!hasFocus) {
                saveOverlayAlpha();
            }
        });

        connectBtn.setOnClickListener(v -> {
            if (!mConnected) {
                attemptConnect();
            } else {
                disconnect();
            }
        });

        scanBtn.setOnClickListener(v -> startScan());

        mMockListener = this::onMockSwitchChanged;
        mOverlayListener = this::onOverlaySwitchChanged;
        mockSwitch.setOnCheckedChangeListener(mMockListener);
        overlaySwitch.setOnCheckedChangeListener(mOverlayListener);
        setSwitchSilently(overlaySwitch, mPrefs.getBoolean("overlay_enabled", true));

        Permission.checkNotificationPermission(this);

        // 如果服务已在运行（例如从悬浮窗点击回到应用），重新绑定以同步状态
        Intent serviceIntent = new Intent(this, MockLocationService.class);
        bindService(serviceIntent, mConnection, BIND_AUTO_CREATE);
    }

    @Override
    protected void onDestroy() {
        if (mBound) {
            if (mBinder != null) mBinder.setListener(null);
            unbindService(mConnection);
            mBound = false;
            mBinder = null;
        }
        super.onDestroy();
    }

    @Override
    public void onBackPressed() {
        if (mConnected) {
            new AlertDialog.Builder(this)
                    .setTitle(R.string.exit_title)
                    .setMessage(R.string.exit_message)
                    .setNegativeButton(R.string.cancel, null)
                    .setPositiveButton(R.string.ok, (d, w) -> {
                        disconnect();
                        finish();
                    })
                    .show();
        } else {
            super.onBackPressed();
        }
    }

    // -------------------- 连接 --------------------

    private void attemptConnect() {
        String ip = ipInput.getText().toString().trim();
        String portStr = portInput.getText().toString().trim();
        if (portStr.isEmpty()) portStr = String.valueOf(MockLocationService.DEFAULT_PORT);

        int port;
        try {
            port = Integer.parseInt(portStr);
        } catch (Exception e) {
            Toast.makeText(this, R.string.te1, Toast.LENGTH_SHORT).show();
            return;
        }
        if (ip.isEmpty()) {
            Toast.makeText(this, R.string.te2, Toast.LENGTH_SHORT).show();
            return;
        }

        mPrefs.edit().putString("ip", ip).putString("port", String.valueOf(port)).apply();
        saveSettings();

        mConnectedIp = ip;
        mConnectedPort = port;
        mConnecting = true;
        connectBtn.setEnabled(false);
        connectBtn.setText(R.string.conning);
        loadingAnim.setVisibility(View.VISIBLE);
        statusText.setText(R.string.conning);

        Intent serviceIntent = new Intent(this, MockLocationService.class);
        serviceIntent.putExtra(MockLocationService.EXTRA_IP, ip);
        serviceIntent.putExtra(MockLocationService.EXTRA_PORT, port);
        ContextCompat.startForegroundService(this, serviceIntent);

        if (!mBound) {
            bindService(serviceIntent, mConnection, BIND_AUTO_CREATE);
        }
    }

    private void disconnect() {
        if (mBound) {
            if (mBinder != null) mBinder.setListener(null);
            unbindService(mConnection);
            mBound = false;
            mBinder = null;
        }
        stopService(new Intent(this, MockLocationService.class));
        mConnected = false;
        resetToDisconnected();
    }

    private void resetToDisconnected() {
        mConnecting = false;
        connectBtn.setEnabled(true);
        connectBtn.setText(R.string.gogogo);
        loadingAnim.setVisibility(View.GONE);
        setSwitchSilently(mockSwitch, false);
        mockSwitch.setEnabled(false);
        overlaySwitch.setEnabled(true);
        setSwitchSilently(overlaySwitch, false);
        statusText.setText(R.string.noconnok);
    }

    private void syncFromService() {
        if (mBinder == null) return;
        mConnected = mBinder.isConnected();
        if (mConnecting && !mConnected) {
            // 仍在连接中，保持"连接中"界面，等待服务回调
            return;
        }
        setSwitchSilently(mockSwitch, mBinder.isMocking());
        if (MockLocationHelper.isOverlayAllowed(this)) {
            if (mPrefs.getBoolean("overlay_enabled", true) && !mBinder.isOverlayVisible()) {
                mBinder.showOverlay();
            }
            setSwitchSilently(overlaySwitch, mBinder.isOverlayVisible());
        } else {
            setSwitchSilently(overlaySwitch, false);
        }
        if (mConnected) {
            connectBtn.setEnabled(true);
            connectBtn.setText(R.string.disconnect);
            loadingAnim.setVisibility(View.GONE);
            mockSwitch.setEnabled(true);
            overlaySwitch.setEnabled(true);
            updateStatusText();
        } else {
            mConnecting = false;
            connectBtn.setEnabled(true);
            connectBtn.setText(R.string.gogogo);
            loadingAnim.setVisibility(View.GONE);
            mockSwitch.setEnabled(false);
            setSwitchSilently(mockSwitch, false);
            overlaySwitch.setEnabled(true);
            statusText.setText(R.string.noconnok);
        }
    }

    // -------------------- 模拟定位开关 --------------------

    private void onMockSwitchChanged(CompoundButton button, boolean checked) {
        saveSettings();
        if (checked) {
            if (!mConnected || mBinder == null) {
                setSwitchSilently(mockSwitch, false);
                Toast.makeText(this, R.string.mock_need_conn, Toast.LENGTH_SHORT).show();
                return;
            }
            if (!MockLocationHelper.isMockLocationAllowed(this)) {
                setSwitchSilently(mockSwitch, false);
                showMockPermissionDialog();
                return;
            }
            maybeRequestIgnoreBatteryOptimization();
            mBinder.startMock();
        } else {
            if (mBinder != null) mBinder.stopMock();
        }
    }

    // 保存平滑补偿设置（注入频率 5~1000Hz，平滑系数 0.05~1.0）
    private void saveSettings() {
        try {
            int hz = Integer.parseInt(injectHzInput.getText().toString().trim());
            hz = Math.max(5, Math.min(1000, hz));
            mPrefs.edit().putInt("inject_hz", hz).apply();
        } catch (Exception ignored) {
        }
        try {
            float ha = Float.parseFloat(headingAlphaInput.getText().toString().trim());
            ha = Math.max(0.05f, Math.min(1f, ha));
            mPrefs.edit().putFloat("heading_alpha", ha).apply();
        } catch (Exception ignored) {
        }
        try {
            float sa = Float.parseFloat(speedAlphaInput.getText().toString().trim());
            sa = Math.max(0.05f, Math.min(1f, sa));
            mPrefs.edit().putFloat("speed_alpha", sa).apply();
        } catch (Exception ignored) {
        }
    }

    // 保存悬浮窗透明度（0.1~1.0），悬浮窗显示中立即生效
    private void saveOverlayAlpha() {
        try {
            float a = Float.parseFloat(overlayAlphaInput.getText().toString().trim());
            a = Math.max(0.1f, Math.min(1f, a));
            mPrefs.edit().putFloat("overlay_alpha", a).apply();
            if (mBinder != null) mBinder.setOverlayAlpha(a);
        } catch (Exception ignored) {
        }
    }

    private void showMockPermissionDialog() {
        new AlertDialog.Builder(this)
                .setTitle(R.string.mock_permission_title)
                .setMessage(R.string.mock_permission_message)
                .setNegativeButton(R.string.cancel, null)
                .setPositiveButton(R.string.settings, (d, w) -> {
                    try {
                        startActivity(new Intent(Settings.ACTION_APPLICATION_DEVELOPMENT_SETTINGS));
                    } catch (Exception ignored) {
                    }
                })
                .show();
    }

    private void maybeRequestIgnoreBatteryOptimization() {
        if (mPrefs.getBoolean("battery_prompted", false)) return;
        PowerManager pm = (PowerManager) getSystemService(POWER_SERVICE);
        if (pm != null && !pm.isIgnoringBatteryOptimizations(getPackageName())) {
            mPrefs.edit().putBoolean("battery_prompted", true).apply();
            try {
                Intent intent = new Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS,
                        Uri.parse("package:" + getPackageName()));
                startActivityForResult(intent, REQ_IGNORE_BATTERY);
            } catch (Exception ignored) {
            }
        }
    }

    // -------------------- 悬浮窗开关 --------------------

    private void onOverlaySwitchChanged(CompoundButton button, boolean checked) {
        if (checked) {
            if (!MockLocationHelper.isOverlayAllowed(this)) {
                setSwitchSilently(overlaySwitch, false);
                showOverlayPermissionDialog();
                return;
            }
            mPrefs.edit().putBoolean("overlay_enabled", true).apply();
            if (mBinder == null) {
                Intent intent = new Intent(this, MockLocationService.class)
                        .setAction(MockLocationService.ACTION_OVERLAY_ONLY);
                ContextCompat.startForegroundService(this, intent);
                bindService(new Intent(this, MockLocationService.class), mConnection, BIND_AUTO_CREATE);
            } else {
                mBinder.showOverlay();
            }
        } else {
            mPrefs.edit().putBoolean("overlay_enabled", false).apply();
            if (mBinder != null) mBinder.hideOverlay();
        }
    }

    private void showOverlayPermissionDialog() {
        new AlertDialog.Builder(this)
                .setTitle(R.string.overlay_permission_title)
                .setMessage(R.string.overlay_permission_message)
                .setNegativeButton(R.string.cancel, null)
                .setPositiveButton(R.string.settings, (d, w) -> {
                    try {
                        Intent intent = new Intent(Settings.ACTION_MANAGE_OVERLAY_PERMISSION,
                                Uri.parse("package:" + getPackageName()));
                        startActivityForResult(intent, REQ_OVERLAY_PERMISSION);
                    } catch (Exception ignored) {
                    }
                })
                .show();
    }

    // -------------------- 状态显示 --------------------

    private void updateStatusText() {
        if (!mConnected) {
            statusText.setText(R.string.noconnok);
            return;
        }

        StringBuilder sb = new StringBuilder();
        sb.append(getString(R.string.connok)).append(" ")
                .append(mConnectedIp).append(":").append(mConnectedPort);
        sb.append("\n").append(mockSwitch.isChecked() ? getString(R.string.mock_on) : getString(R.string.mock_off));
        if (mBinder != null && mBinder.getLatencyMs() >= 0) {
            sb.append(String.format("\n%s", getString(R.string.main_latency, mBinder.getLatencyMs())));
        }

        MockLocationService.FlightData d = mBinder != null ? mBinder.getFlightData() : null;
        if (d != null && d.valid) {
            String latDir = d.lat >= 0 ? "N" : "S";
            String lonDir = d.lon >= 0 ? "E" : "W";
            sb.append("\n\n飞机数据\n");
            sb.append(String.format("纬度: %.6f %s\n", Math.abs(d.lat), latDir));
            sb.append(String.format("经度: %.6f %s\n", Math.abs(d.lon), lonDir));
            sb.append(String.format("高度: %.1f ft\n", d.altitudeFt));
            sb.append(String.format("航向: %.1f°\n", d.heading));
            sb.append(String.format("俯仰: %.1f°\n", d.pitch));
            sb.append(String.format("横滚: %.1f°\n", d.roll));
            sb.append(String.format("地速: %.2f m/s\n", d.groundSpeed));
            sb.append(String.format("空速: %.2f m/s", d.airSpeed));
        } else {
            sb.append("\n").append(getString(R.string.unpos));
        }
        statusText.setText(sb.toString());
    }

    private void setSwitchSilently(Switch sw, boolean checked) {
        CompoundButton.OnCheckedChangeListener listener =
                sw == mockSwitch ? mMockListener : mOverlayListener;
        sw.setOnCheckedChangeListener(null);
        sw.setChecked(checked);
        sw.setOnCheckedChangeListener(listener);
    }

    // -------------------- 扫码连接 --------------------

    private void startScan() {
        if (Permission.isPermissionGranted(this)) {
            launchScanner();
        } else {
            mPendingScan = true;
            Permission.checkPermission(this);
        }
    }

    private void launchScanner() {
        ScanCodeConfig.create(this)
                .setStyle(ScanStyle.WECHAT)
                .setPlayAudio(true)
                .setLimitRect(true)
                .setScanSize(1000, 0, 0)
                .setShowFrame(true)
                .setFrameRadius(12)
                .setFrameWith(4)
                .setFrameLength(25)
                .setShowShadow(true)
                .setScanMode(ScanMode.REVERSE)
                .setIdentifyMultiple(true)
                .setQrCodeHintDrawableWidth(120)
                .setQrCodeHintDrawableHeight(120)
                .setStartCodeHintAnimation(true)
                .setQrCodeHintAlpha(0.5f)
                .buidler()
                .start(ScanActivity.class);
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode == Permission.REQUEST_CODE && mPendingScan) {
            mPendingScan = false;
            boolean granted = grantResults != null && grantResults.length > 0;
            if (granted) {
                for (int r : grantResults) {
                    if (r != PackageManager.PERMISSION_GRANTED) {
                        granted = false;
                        break;
                    }
                }
            }
            if (granted) {
                launchScanner();
            } else {
                Toast.makeText(this, R.string.camera_permission_denied, Toast.LENGTH_LONG).show();
            }
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, @Nullable Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (resultCode == RESULT_OK && data != null) {
            if (requestCode == ScanCodeConfig.QUESTCODE) {
                Bundle extras = data.getExtras();
                if (extras != null) {
                    parseQRCode(extras.getString(ScanCodeConfig.CODE_KEY));
                }
            } else if (requestCode == ALBUM_QUEST_CODE) {
                String code = ScanCodeConfig.scanningImage(this, data.getData());
                parseQRCode(code);
            }
        }
        if (requestCode == REQ_OVERLAY_PERMISSION) {
            if (MockLocationHelper.isOverlayAllowed(this)) {
                mPrefs.edit().putBoolean("overlay_enabled", true).apply();
                if (mBinder != null) {
                    mBinder.showOverlay();
                    setSwitchSilently(overlaySwitch, true);
                } else {
                    Intent intent = new Intent(this, MockLocationService.class)
                            .setAction(MockLocationService.ACTION_OVERLAY_ONLY);
                    ContextCompat.startForegroundService(this, intent);
                    bindService(new Intent(this, MockLocationService.class), mConnection, BIND_AUTO_CREATE);
                }
            }
        }
    }

    // 解析电脑端二维码：内容格式为 IP:端口 或 [IPv6]:端口（兼容旧格式 ;PAIR:xxxx）
    private void parseQRCode(String content) {
        if (content == null || content.isEmpty()) {
            Toast.makeText(this, R.string.qrerr, Toast.LENGTH_SHORT).show();
            return;
        }
        try {
            String[] parts = content.split(";");
            String addrPort = parts[0].trim();
            String ip;
            int port;
            if (addrPort.startsWith("[")) {
                int close = addrPort.indexOf(']');
                if (close <= 0) throw new Exception();
                ip = addrPort.substring(1, close);
                String portPart = addrPort.substring(close + 1);
                if (portPart.startsWith(":")) portPart = portPart.substring(1);
                port = Integer.parseInt(portPart);
            } else {
                int lastColon = addrPort.lastIndexOf(':');
                if (lastColon <= 0) throw new Exception();
                ip = addrPort.substring(0, lastColon);
                port = Integer.parseInt(addrPort.substring(lastColon + 1));
            }
            ipInput.setText(ip);
            portInput.setText(String.valueOf(port));
            attemptConnect();
        } catch (Exception e) {
            Toast.makeText(this, R.string.qrerr, Toast.LENGTH_SHORT).show();
        }
    }

    // -------------------- StatusListener --------------------

    @Override
    public void onConnectionChanged(boolean connected) {
        mConnected = connected;
        mConnecting = false;
        connectBtn.setEnabled(true);
        if (connected) {
            connectBtn.setText(R.string.disconnect);
            loadingAnim.setVisibility(View.GONE);
            mockSwitch.setEnabled(true);
            overlaySwitch.setEnabled(true);
            autoStartMock();
        } else {
            connectBtn.setText(R.string.gogogo);
            loadingAnim.setVisibility(View.GONE);
            mockSwitch.setEnabled(false);
            setSwitchSilently(mockSwitch, false);
            if (mBinder != null) setSwitchSilently(overlaySwitch, mBinder.isOverlayVisible());
            statusText.setText(R.string.noconnok);
        }
        updateStatusText();
    }

    // 连接成功后自动开启模拟定位（手动关闭后本次连接内不再自动开，重连后再次自动开）
    private void autoStartMock() {
        if (!mConnected || mBinder == null) return;
        saveSettings();
        if (!MockLocationHelper.isMockLocationAllowed(this)) {
            showMockPermissionDialog();
            return;
        }
        maybeRequestIgnoreBatteryOptimization();
        mBinder.startMock();
    }

    @Override
    public void onMockChanged(boolean mocking) {
        setSwitchSilently(mockSwitch, mocking);
        updateStatusText();
    }

    @Override
    public void onData(MockLocationService.FlightData data) {
        updateStatusText();
    }

    @Override
    public void onLatencyChanged(int latencyMs) {
        updateStatusText();
    }

    @Override
    public void onOverlayChanged(boolean visible) {
        setSwitchSilently(overlaySwitch, visible);
    }

    @Override
    public void onError(String message) {
        if (message != null && !message.isEmpty()) {
            Toast.makeText(this, message, Toast.LENGTH_LONG).show();
        }
        if (!mConnected) {
            statusText.setText(message != null ? message : getString(R.string.connfail));
        }
    }
}
