/********************************************************************
 * MSFS-SimGPStoAndroid 安卓端 - 模拟定位相关工具
 * 参考 ZCShou/GoGoGo 的判断方式：直接尝试注册模拟提供者来验证权限
 * 本软件遵循 CC BY-NC-SA 4.0 协议，不得用于商业用途！
 ********************************************************************/

package com.msfs.simconnect.client;

import android.annotation.SuppressLint;
import android.content.Context;
import android.location.Criteria;
import android.location.LocationManager;
import android.location.provider.ProviderProperties;
import android.os.Build;
import android.provider.Settings;

import java.util.List;

public class MockLocationHelper {

    /**
     * 判断当前应用是否已被授予模拟定位权限
     * （即是否已在 开发者选项 -> 选择模拟位置信息应用 中选择本应用）
     */
    @SuppressLint("WrongConstant")
    public static boolean isMockLocationAllowed(Context context) {
        try {
            LocationManager lm = (LocationManager) context.getSystemService(Context.LOCATION_SERVICE);
            List<String> providers = lm.getAllProviders();
            if (providers == null || !providers.contains(LocationManager.GPS_PROVIDER)) {
                return false;
            }
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                lm.addTestProvider(LocationManager.GPS_PROVIDER, false, true, false,
                        false, true, true, true,
                        ProviderProperties.POWER_USAGE_HIGH, ProviderProperties.ACCURACY_FINE);
            } else {
                lm.addTestProvider(LocationManager.GPS_PROVIDER, false, true, false,
                        false, true, true, true,
                        Criteria.POWER_HIGH, Criteria.ACCURACY_FINE);
            }
            lm.setTestProviderEnabled(LocationManager.GPS_PROVIDER, false);
            lm.removeTestProvider(LocationManager.GPS_PROVIDER);
            return true;
        } catch (SecurityException | IllegalArgumentException e) {
            return false;
        }
    }

    /** 判断是否已授予悬浮窗权限（显示在其他应用上层） */
    public static boolean isOverlayAllowed(Context context) {
        return Settings.canDrawOverlays(context);
    }
}
