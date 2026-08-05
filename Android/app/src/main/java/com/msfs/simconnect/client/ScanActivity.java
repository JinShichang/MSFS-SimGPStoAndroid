/********************************************************************
 * MSFS-SimGPStoAndroid 安卓端
 * 时间: 2025-12-05
 * 本软件遵循 CC BY-NC-SA 4.0 协议，不得用于商业用途！
 ********************************************************************/

package com.msfs.simconnect.client;

import android.os.Bundle;
import android.os.PersistableBundle;
import android.view.View;
import android.widget.ImageButton;

import androidx.annotation.Nullable;

import com.yxing.ScanCodeActivity;

public class ScanActivity extends ScanCodeActivity {
    private ImageButton btnOpenFlash;
    private boolean isOpenFlash;

    @Override
    public void onCreate(@Nullable Bundle savedInstanceState, @Nullable PersistableBundle persistentState) {
        super.onCreate(savedInstanceState, persistentState);
        setContentView(R.layout.activity_scan);
        Permission.checkPermission(this);
    }

    @Override
    public int getLayoutId() {
        return R.layout.activity_scan;
    }

    @Override
    public void initData() {
        super.initData();
        isOpenFlash = false;
        btnOpenFlash = findViewById(R.id.btn_openflash);
        btnOpenFlash.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View view) {
                isOpenFlash = !isOpenFlash;
                setFlashStatus(isOpenFlash);
            }
        });
        ImageButton back_btn = findViewById(R.id.back_btn);
        back_btn.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View view) {
                finish();
            }
        });
    }
}