/********************************************************************
 * MSFS-SimGPStoAndroid 安卓端 - 状态悬浮窗
 * 显示连接/模拟状态与实时坐标、速度、高度，可拖动，可关闭
 * 参考 ZCShou/GoGoGo 的悬浮窗实现（TYPE_APPLICATION_OVERLAY）
 * 本软件遵循 CC BY-NC-SA 4.0 协议，不得用于商业用途！
 ********************************************************************/

package com.msfs.simconnect.client;

import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;
import android.graphics.PixelFormat;
import android.os.Handler;
import android.os.Looper;
import android.view.Gravity;
import android.view.LayoutInflater;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowManager;
import android.widget.TextView;

public class StatusOverlay {

    public interface Callback {
        void onVisibilityChanged(boolean visible);
    }

    private final Context mContext;
    private final WindowManager mWindowManager;
    private final Handler mMainHandler = new Handler(Looper.getMainLooper());

    private View mView;
    private WindowManager.LayoutParams mParams;
    private TextView mStatusText;
    private TextView mCoordText;
    private TextView mSpeedText;
    private TextView mHeadingText;
    private Callback mCallback;
    private float mAlpha = 0.9f;

    private int mTouchStartX;
    private int mTouchStartY;
    private int mStartParamX;
    private int mStartParamY;
    private boolean mDragging;

    public StatusOverlay(Context context) {
        mContext = context;
        mWindowManager = (WindowManager) context.getSystemService(Context.WINDOW_SERVICE);
    }

    public void setCallback(Callback callback) {
        mCallback = callback;
    }

    public void show() {
        if (mView != null && mView.getParent() != null) return;

        mView = LayoutInflater.from(mContext).inflate(R.layout.overlay_status, null);
        mStatusText = mView.findViewById(R.id.overlayStatus);
        mCoordText = mView.findViewById(R.id.overlayCoord);
        mSpeedText = mView.findViewById(R.id.overlaySpeed);
        mHeadingText = mView.findViewById(R.id.overlayHeading);

        mParams = new WindowManager.LayoutParams();
        mParams.type = WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY;
        mParams.format = PixelFormat.TRANSLUCENT;
        mParams.alpha = mAlpha;
        mParams.flags = WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                | WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL
                | WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN;
        mParams.gravity = Gravity.START | Gravity.TOP;
        mParams.width = WindowManager.LayoutParams.WRAP_CONTENT;
        mParams.height = WindowManager.LayoutParams.WRAP_CONTENT;
        mParams.x = 48;
        mParams.y = 240;

        mView.setOnTouchListener(this::onTouch);
        try {
            mWindowManager.addView(mView, mParams);
        } catch (Exception e) {
            mView = null;
            if (mCallback != null) mCallback.onVisibilityChanged(false);
            return;
        }
        if (mCallback != null) mCallback.onVisibilityChanged(true);
    }

    public void hide() {
        if (mView != null && mView.getParent() != null) {
            mWindowManager.removeView(mView);
        }
        mView = null;
        if (mCallback != null) mCallback.onVisibilityChanged(false);
    }

    public void destroy() {
        hide();
    }

    public boolean isShowing() {
        return mView != null && mView.getParent() != null;
    }

    /** 调节悬浮窗透明度（0.1~1.0），悬浮窗显示中实时生效 */
    public void setAlpha(float alpha) {
        mAlpha = Math.max(0.1f, Math.min(1f, alpha));
        if (mView != null && mView.getParent() != null && mParams != null) {
            mParams.alpha = mAlpha;
            mWindowManager.updateViewLayout(mView, mParams);
        }
    }

    public void update(boolean connected, boolean mocking, MockLocationService.FlightData data, long latencyMs) {
        mMainHandler.post(() -> {
            if (mView == null) return;
            String status;
            if (!connected) {
                status = mContext.getString(R.string.overlay_disconnected);
            } else {
                status = mocking
                        ? mContext.getString(R.string.overlay_mocking)
                        : mContext.getString(R.string.overlay_connected);
                if (latencyMs >= 0) {
                    status += " · " + String.format(mContext.getString(R.string.overlay_latency), latencyMs);
                }
            }
            mStatusText.setText(status);

            if (data != null && data.valid) {
                mCoordText.setText(data.coordsText());
                mSpeedText.setText(String.format(
                        mContext.getString(R.string.overlay_speed_gs_as),
                        data.groundSpeed * 3.6f,
                        data.airSpeed * 3.6f));
                mHeadingText.setText(String.format(
                        mContext.getString(R.string.overlay_heading_alt),
                        Math.round(data.heading) % 360,
                        directionFromDegrees(data.heading),
                        data.altitudeFt * 0.3048));
            } else {
                mCoordText.setText(mContext.getString(R.string.overlay_no_coord));
                mSpeedText.setText(mContext.getString(R.string.overlay_no_speed));
                mHeadingText.setText(mContext.getString(R.string.overlay_no_heading));
            }
        });
    }

    private static String directionFromDegrees(float deg) {
        int d = ((int) Math.round(deg) % 360 + 360) % 360;
        String[] dirs = {"北", "东北", "东", "东南", "南", "西南", "西", "西北"};
        return dirs[((d + 22) / 45) % 8];
    }

    private boolean onTouch(View v, MotionEvent event) {
        switch (event.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
                mDragging = false;
                mTouchStartX = (int) event.getRawX();
                mTouchStartY = (int) event.getRawY();
                mStartParamX = mParams.x;
                mStartParamY = mParams.y;
                return true;
            case MotionEvent.ACTION_MOVE:
                int dx = (int) event.getRawX() - mTouchStartX;
                int dy = (int) event.getRawY() - mTouchStartY;
                if (Math.abs(dx) + Math.abs(dy) > 10) mDragging = true;
                if (mDragging) {
                    mParams.x = mStartParamX + dx;
                    mParams.y = mStartParamY + dy;
                    mWindowManager.updateViewLayout(mView, mParams);
                }
                return true;
            case MotionEvent.ACTION_UP:
                if (!mDragging) openApp();
                return true;
            default:
                return false;
        }
    }

    private void openApp() {
        try {
            Intent intent = new Intent(mContext, MainActivity.class);
            intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            PendingIntent pi = PendingIntent.getActivity(mContext, 0, intent,
                    PendingIntent.FLAG_IMMUTABLE);
            pi.send();
        } catch (PendingIntent.CanceledException ignored) {
        }
    }
}
