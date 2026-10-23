// Stub <android/asset_manager_jni.h> untuk kompilasi di PC.
#pragma once
#include <jni.h>
#include <android/asset_manager.h>
#ifdef __cplusplus
extern "C" {
#endif
AAssetManager* AAssetManager_fromJava(JNIEnv* env, jobject assetManager);
#ifdef __cplusplus
}
#endif
