/********************************************************************
 * MSFS-SimGPStoAndroid 安卓端
 * 时间: 2025-12-05
 * 本软件遵循 CC BY-NC-SA 4.0 协议，不得用于商业用途！
 ********************************************************************/

package com.msfs.simconnect.client;

import android.Manifest;
import android.app.Activity;
import android.content.pm.PackageManager;
import android.os.Build;
import androidx.core.app.ActivityCompat;
import androidx.core.content.ContextCompat;




//用于授权相机、通知等权限
public class Permission {
    public static final int REQUEST_CODE = 5;
    public static final int REQUEST_NOTIFICATION_CODE = 6;
    // 扫码只需要相机权限（Android 13+ 相册选择走系统照片选择器，无需存储权限）
    private static final String[] permission = new String[]{
            Manifest.permission.CAMERA,
    };
    //每个权限是否已授
    public static boolean isPermissionGranted(Activity activity){
        if(Build.VERSION.SDK_INT >= 23){
            for(int i = 0; i < permission.length;i++) {
                int checkPermission = ContextCompat.checkSelfPermission(activity,permission[i]);
                /***
                 * checkPermission返回两个值
                 * 有权限: PackageManager.PERMISSION_GRANTED
                 * 无权限: PackageManager.PERMISSION_DENIED
                 */
                if(checkPermission != PackageManager.PERMISSION_GRANTED){
                    return false;
                }
            }
            return true;
        }else{
            return true;
        }
    }

    public static boolean checkPermission(Activity activity){
        if(isPermissionGranted(activity)) {
            return true;
        } else {
            //如果没有设置过权限许可，则弹出系统的授权窗口
            ActivityCompat.requestPermissions(activity,permission,REQUEST_CODE);
            return false;
        }
    }

    // Android 13+ 需要通知权限，前台服务通知才会显示
    public static void checkNotificationPermission(Activity activity) {
        if (Build.VERSION.SDK_INT >= 33) {
            if (ContextCompat.checkSelfPermission(activity, Manifest.permission.POST_NOTIFICATIONS)
                    != PackageManager.PERMISSION_GRANTED) {
                ActivityCompat.requestPermissions(activity,
                        new String[]{Manifest.permission.POST_NOTIFICATIONS},
                        REQUEST_NOTIFICATION_CODE);
            }
        }
    }
}
