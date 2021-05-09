#include "mbed.h"
#include "mbed_rpc.h"
#include "accelerometer_handler.h"
#include "config.h"
#include "magic_wand_model_data.h"
#include "uLCD_4DGL.h"
#include "stm32l475e_iot01_accelero.h"
#include "MQTTNetwork.h"
#include "MQTTmbed.h"
#include "MQTTClient.h"


#include "tensorflow/lite/c/common.h"
#include "tensorflow/lite/micro/kernels/micro_ops.h"
#include "tensorflow/lite/micro/micro_error_reporter.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"
#include "tensorflow/lite/version.h"

int16_t DataXYZ1[3] = {0};
int16_t DataXYZ2[3] = {0};
int16_t vector[3];
WiFiInterface *wifi;
InterruptIn btn2(USER_BUTTON);
constexpr int kTensorArenaSize = 60 * 1024;
uint8_t tensor_arena[kTensorArenaSize];

int measure = 0;
int threshold_angle = 30;

bool flag1 = 1;
bool flag2 = 1;
bool flag3 = true;
int num = 1;

uLCD_4DGL uLCD(D1, D0, D2);
DigitalOut myled1(LED1);
DigitalOut myled2(LED2);
DigitalOut myled3(LED3);
BufferedSerial pc(USBTX, USBRX);

void gesture_ui(Arguments *in, Reply *out);
void tiltangle(Arguments *in, Reply *out);
RPCFunction Gesture_ui(&gesture_ui, "Gesture_ui");
RPCFunction Tilt_angle(&tiltangle, "Tilt_angle");

double x, y;
void ML();
void mqtt();
void detect();
volatile int message_num = 0;
volatile int arrivedcount = 0;
volatile bool closed = false;

const char* topic = "Mbed";
double vectorvalue = 0;

Thread mqtt_thread(osPriorityHigh);
EventQueue mqtt_queue;

Thread thread_gesture;
Thread thread_detect;
Thread thread_mqtt;

int main() {
    thread_mqtt.start(mqtt);
    thread_detect.start(detect);
    thread_gesture.start(ML);
    
    char buf[256], outbuf[256];
    myled1 = 0; 
    myled2 = 0; 
    myled3 = 0; 

    BSP_ACCELERO_Init();

    FILE *devin = fdopen(&pc, "r");
    FILE *devout = fdopen(&pc, "w");

    while(1) {
        memset(buf, 0, 256);
        for (int i = 0; ; i++) {
            char recv = fgetc(devin);
            if (recv == '\n') {
                printf("\r\n");
                break;
            }
            buf[i] = fputc(recv, devout);
        }
        //Call the static call method on the RPC class
        RPC::call(buf, outbuf);
        printf("%s\r\n", outbuf);
    }
}
void tiltangle(Arguments *in, Reply *out) {
    char buffer[256], outbuf[256];
    char strings[20];
    x = in->getArg<int>();
    if (x == 1){
        flag2 = 0;
        
        myled2 = 1; 
        out->putData("tiltangle_mode");
        flag3 = true;
        
    } else if (x == 0) {
        flag2 = 1;
        myled2 = 0;; 
        out->putData("back to rpc");
    }
}
void gesture_ui (Arguments *in, Reply *out) {
    char buffer[256], outbuf[256];
    char strings[20];
    x = in->getArg<int>();
    if (x == 1){
        flag1 = 0;
        myled1 = 1; 
        out->putData("gesture_ui_mode");
        
    } else if (x == 0) {
        flag1 = 1;
        myled1 = 0; 
        out->putData("back to rpc");
    }
}
int PredictGesture(float* output) {
  // How many times the most recent gesture has been matched in a row
  static int continuous_count = 0;
  // The result of the last prediction
  static int last_predict = -1;

  // Find whichever output has a probability > 0.8 (they sum to 1)
  int this_predict = -1;
  for (int i = 0; i < label_num; i++) {
    if (output[i] > 0.8) this_predict = i;
  }

  // No gesture was detected above the threshold
  if (this_predict == -1) {
    continuous_count = 0;
    last_predict = label_num;
    return label_num;
  }

  if (last_predict == this_predict) {
    continuous_count += 1;
  } else {
    continuous_count = 0;
  }
  last_predict = this_predict;

  // If we haven't yet had enough consecutive matches for this gesture,
  // report a negative result
  if (continuous_count < config.consecutiveInferenceThresholds[this_predict]) {
    return label_num;
  }
  // Otherwise, we've seen a positive result, so clear all our variables
  // and report it
  continuous_count = 0;
  last_predict = -1;

  return this_predict;
}
void mqtt() {

    wifi = WiFiInterface::get_default_instance();
    if (!wifi) {
            printf("ERROR: No WiFiInterface found.\r\n");
            return ;
    }
    printf("\nConnecting to %s...\r\n", MBED_CONF_APP_WIFI_SSID);
    int ret = wifi->connect(MBED_CONF_APP_WIFI_SSID, MBED_CONF_APP_WIFI_PASSWORD, NSAPI_SECURITY_WPA_WPA2);
    if (ret != 0) {
            printf("\nConnection error: %d\r\n", ret);
            return ;
    }


    NetworkInterface* net = wifi;
    MQTTNetwork mqttNetwork(net);
    MQTT::Client<MQTTNetwork, Countdown> client(mqttNetwork);

    //TODO: revise host to your IP
    const char* host = "172.20.10.4";
    printf("Connecting to TCP network...\r\n");

    SocketAddress sockAddr;
    sockAddr.set_ip_address(host);
    sockAddr.set_port(1883);

    printf("address is %s/%d\r\n", (sockAddr.get_ip_address() ? sockAddr.get_ip_address() : "None"),  (sockAddr.get_port() ? sockAddr.get_port() : 0) ); //check setting

    int rc = mqttNetwork.connect(sockAddr);//(host, 1883);
    if (rc != 0) {
            printf("Connection error.");
            return ;
    }
    printf("Successfully connected!\r\n");

    MQTTPacket_connectData data = MQTTPacket_connectData_initializer;
    data.MQTTVersion = 3;
    data.clientID.cstring = "Mbed";

    if ((rc = client.connect(data)) != 0){
            printf("Fail to connect MQTT\r\n");
    }
    if (client.subscribe(topic, MQTT::QOS0, messageArrived) != 0){
            printf("Fail to subscribe\r\n");
    }

    mqtt_thread.start(callback(&mqtt_queue, &EventQueue::dispatch_forever));
    btn2.rise(mqtt_queue.event(&publish_message, &client));
    //btn3.rise(&close_mqtt);
    printf("thresh = %d, angle = %d\n", threshold_angle, vectorvalue);


    while (1) {
        if (closed) break;
        ThisThread::sleep_for(100ms);
        if ((vectorvalue > threshold_angle) && (flag3 == false)) {
            printf("Yes\n");
            mqtt_queue.call(&publish_message, &client);
            flag3 = true;
        }
    }
    printf("Ready to close MQTT Network......\n");

    if ((rc = client.unsubscribe(topic)) != 0) {
            printf("Failed: rc from unsubscribe was %d\n", rc);
    }
    if ((rc = client.disconnect()) != 0) {
    printf("Failed: rc from disconnect was %d\n", rc);
    }

    mqttNetwork.disconnect();
    printf("Successfully closed!\n");

}
void detect () {
    double init_angle;
    while (1) {
        if (measure == 0 && flag2 == 0) {
            myled3.write(1); 
            int k = 0;
            while (k < 10) {
                BSP_ACCELERO_AccGetXYZ(DataXYZ2);
                printf("%d, %d, %d\n", DataXYZ2[0], DataXYZ2[1], DataXYZ2[2]);
                vector[0] += DataXYZ2[0]; 
                vector[1] += DataXYZ2[1];
                vector[2] += DataXYZ2[2];
                k++;
            }
            vector[0] = vector[0] /10; 
            vector[1] = vector[1] /10; 
            vector[2] = vector[2] /10; 
            init_angle = atan(vector[0]/vector[2]);
            measure = 1;
            myled3.write(0); 
        }/**
        if (flag2 == 0) {
            BSP_ACCELERO_AccGetXYZ(DataXYZ1);
           
            vectorvalue = DataXYZ1[0]*DataXYZ2[0] + DataXYZ1[1]*DataXYZ2[1] + DataXYZ1[2]*DataXYZ2[2];
            vectorvalue = vectorvalue / sqrt(DataXYZ1[0]*DataXYZ1[0] + DataXYZ1[1]*DataXYZ1[1] + DataXYZ1[2]*DataXYZ1[2]);
            vectorvalue = vectorvalue / sqrt(DataXYZ2[0]*DataXYZ2[0] + DataXYZ2[1]*DataXYZ2[1] + DataXYZ2[2]*DataXYZ2[2]);
            vectorvalue = acos(vectorvalue);
            vectorvalue = vectorvalue/ M_PI * 180;
           
            uLCD.locate(2,2);
            uLCD.printf("%3d",vectorvalue);
            if(vectorvalue > threshold_angle){
                printf("angle is %d: %d\n",num,vectorvalue);
                num++;
                if(num > 10) {
                    flag3 = false;
                    num = 1;
                }
            }
            ThisThread::sleep_for(100ms);
        }
    }*/
void ML(){

    bool should_clear_buffer = false;
    bool got_data = false;

    int gesture_index;
    static tflite::MicroErrorReporter micro_error_reporter;
    tflite::ErrorReporter* error_reporter = &micro_error_reporter;

    const tflite::Model* model = tflite::GetModel(g_magic_wand_model_data);
    if (model->version() != TFLITE_SCHEMA_VERSION) {
        error_reporter->Report(
            "Model provided is schema version %d not equal "
            "to supported version %d.",
            model->version(), TFLITE_SCHEMA_VERSION);
        return ;
    }

    static tflite::MicroOpResolver<6> micro_op_resolver;
    micro_op_resolver.AddBuiltin(
        tflite::BuiltinOperator_DEPTHWISE_CONV_2D,
        tflite::ops::micro::Register_DEPTHWISE_CONV_2D());
    micro_op_resolver.AddBuiltin(tflite::BuiltinOperator_MAX_POOL_2D,
                                tflite::ops::micro::Register_MAX_POOL_2D());
    micro_op_resolver.AddBuiltin(tflite::BuiltinOperator_CONV_2D,
                                tflite::ops::micro::Register_CONV_2D());
    micro_op_resolver.AddBuiltin(tflite::BuiltinOperator_FULLY_CONNECTED,
                                tflite::ops::micro::Register_FULLY_CONNECTED());
    micro_op_resolver.AddBuiltin(tflite::BuiltinOperator_SOFTMAX,
                                tflite::ops::micro::Register_SOFTMAX());
    micro_op_resolver.AddBuiltin(tflite::BuiltinOperator_RESHAPE,
                                tflite::ops::micro::Register_RESHAPE(), 1);

    static tflite::MicroInterpreter static_interpreter(
        model, micro_op_resolver, tensor_arena, kTensorArenaSize, error_reporter);
    tflite::MicroInterpreter* interpreter = &static_interpreter;

    interpreter->AllocateTensors();

    TfLiteTensor* model_input = interpreter->input(0);
    if ((model_input->dims->size != 4) || (model_input->dims->data[0] != 1) ||
        (model_input->dims->data[1] != config.seq_length) ||
        (model_input->dims->data[2] != kChannelNumber) ||
        (model_input->type != kTfLiteFloat32)) {
        error_reporter->Report("Bad input tensor parameters in model");
        return ;
    }

    int input_length = model_input->bytes / sizeof(float);

    TfLiteStatus setup_status = SetupAccelerometer(error_reporter);
    if (setup_status != kTfLiteOk) {
        error_reporter->Report("Set up failed\n");
        return ;
    }

    error_reporter->Report("Set up successful...\n");

    while (1) {
        if (flag1 == 0){
            uLCD.locate(1,2);
            uLCD.printf("%4d",threshold_angle);
            // Attempt to read new data from the accelerometer
            got_data = ReadAccelerometer(error_reporter, model_input->data.f,
                                        input_length, should_clear_buffer);
            // If there was no new data,
            // don't try to clear the buffer again and wait until next time
            if (!got_data) {
            should_clear_buffer = false;
            continue;
            }

            // Run inference, and report any error
            TfLiteStatus invoke_status = interpreter->Invoke();
            if (invoke_status != kTfLiteOk) {
            error_reporter->Report("Invoke failed on index: %d\n", begin_index);
            continue;
            }

            // Analyze the results to obtain a prediction
            gesture_index = PredictGesture(interpreter->output(0)->data.f);

            uLCD.locate(6,5);
            uLCD.printf("%3d",gesture_index);
            // Clear the buffer next time we read data
            should_clear_buffer = gesture_index < label_num;

            if (gesture_index < label_num){
                if(gesture_index == 0 && threshold_angle < 40) threshold_angle += 5;
                else if(gesture_index == 1 && threshold_angle > 30)threshold_angle -= 5;
            }
            ThisThread::sleep_for(100ms);

        } else {
            ThisThread::sleep_for(100ms);
        }
     
    }
}

void messageArrived(MQTT::MessageData& md) {
    MQTT::Message &message = md.message;
    ThisThread::sleep_for(1000ms);
    char payload[300];
    sprintf(payload, " %.*s\r\n", message.payloadlen, (char*)message.payload);
    printf(payload);
    ++arrivedcount;
}

void publish_message(MQTT::Client<MQTTNetwork, Countdown>* client) {
    message_num++;
    MQTT::Message message;
    char buff[100];
    if (flag1 == 0){
        sprintf(buff, "2,%d", threshold_angle);
    } else if (flag2 == 0){
        sprintf(buff, "2,%d", vectorvalue);
    }
    message.qos = MQTT::QOS0;
    message.retained = false;
    message.dup = false;
 
    message.payload = (void*) buff;
    message.payloadlen = strlen(buff) + 1;
    int rc = client->publish(topic, message);
}

void close_mqtt() {
    closed = true;
}



