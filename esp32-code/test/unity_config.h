#ifndef UNITY_CONFIG_H
#define UNITY_CONFIG_H

/*
 * Unity's output, for every suite, in place of the configuration PlatformIO
 * would generate -- which ends a run on Serial.end(). See unity_config.cpp for
 * why that matters here. Its presence in test/ is what stops the generation.
 */

#ifdef __cplusplus
extern "C"
{
#endif

void unityOutputStart(unsigned long);
void unityOutputChar(unsigned int);
void unityOutputFlush(void);
void unityOutputComplete(void);

#define UNITY_OUTPUT_START()    unityOutputStart((unsigned long) 115200)
#define UNITY_OUTPUT_CHAR(c)    unityOutputChar(c)
#define UNITY_OUTPUT_FLUSH()    unityOutputFlush()
#define UNITY_OUTPUT_COMPLETE() unityOutputComplete()

#ifdef __cplusplus
}
#endif

#endif  // UNITY_CONFIG_H
