#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <esp_log.h>
#include "esp_system.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h> // Thêm thư viện để sử dụng hàm exp

extern const uint8_t model_0_bst_start[] asm("_binary_model_0_bst_start");
extern const uint8_t model_0_bst_end[] asm("_binary_model_0_bst_end");

// Thêm struct để mô phỏng FILE* với memory buffer
typedef struct {
    const uint8_t* buffer;
    size_t size;
    size_t position;
} MemoryBuffer;

// Hàm đọc từ memory buffer
size_t memory_fread(void* ptr, size_t size, size_t count, MemoryBuffer* buffer) {
    size_t total_size = size * count;
    if (buffer->position + total_size > buffer->size) {
        total_size = buffer->size - buffer->position;
        count = total_size / size;
    }
    if (total_size > 0) {
        memcpy(ptr, buffer->buffer + buffer->position, total_size);
        buffer->position += total_size;
    }
    return count;
}

// Định nghĩa cấu trúc cho nút của cây
typedef struct TreeNode {
    unsigned int feature_id;     // ID thuộc tính để phân chia
    double split_point;          // Điểm phân chia
    double gain;                 // Giá trị gain
    struct TreeNode* left;       // Con trỏ đến nút con trái
    struct TreeNode* right;      // Con trỏ đến nút con phải
} TreeNode;

// Hàm để đọc cây từ tệp nhị phân
TreeNode* loadTreeFromMemory(MemoryBuffer* buffer) {
    unsigned char flag;
    if (memory_fread(&flag, sizeof(flag), 1, buffer) != 1) {
        return NULL;
    }

    TreeNode* node = (TreeNode*)malloc(sizeof(TreeNode));
    if (node == NULL) {
        return NULL;
    }

    if (flag == 1) {
        double leaf_value;
        if (memory_fread(&leaf_value, sizeof(leaf_value), 1, buffer) != 1) {
            free(node);
            return NULL;
        }
        node->feature_id = 0;
        node->split_point = leaf_value;
        node->gain = 0;
        node->left = NULL;
        node->right = NULL;
    } else {
        if (memory_fread(&node->feature_id, sizeof(node->feature_id), 1, buffer) != 1 ||
            memory_fread(&node->split_point, sizeof(node->split_point), 1, buffer) != 1 ||
            memory_fread(&node->gain, sizeof(node->gain), 1, buffer) != 1) {
            free(node);
            return NULL;
        }
        node->left = loadTreeFromMemory(buffer);
        node->right = loadTreeFromMemory(buffer);
    }
    return node;
}

// Định nghĩa cấu trúc cho cây phân loại
typedef struct MyXGBClassificationTree {
    int max_depth;             // Độ sâu tối đa
    double reg_lambda;         // Hệ số điều chỉnh
    double prune_gamma;        // Ngưỡng cắt tỉa
    TreeNode* root;            // Nút gốc
} MyXGBClassificationTree;

// Hàm khởi tạo cây phân loại
MyXGBClassificationTree* createMyXGBClassificationTree(TreeNode* root, int max_depth, double reg_lambda, double prune_gamma) {
    MyXGBClassificationTree* tree = (MyXGBClassificationTree*)malloc(sizeof(MyXGBClassificationTree));
    if (tree == NULL) {
        return NULL; // Kiểm tra cấp phát bộ nhớ
    }
    tree->max_depth = max_depth;
    tree->reg_lambda = reg_lambda;
    tree->prune_gamma = prune_gamma;
    tree->root = root;
    return tree;
}

// Hàm để giải phóng bộ nhớ cho cây
void freeTree(TreeNode* node) {
    if (node == NULL) return; // Nếu nút là NULL thì không làm gì
    freeTree(node->left);     // Giải phóng cây con trái
    freeTree(node->right);    // Giải phóng cây con phải
    free(node);               // Giải phóng nút hiện tại
}

// Hàm để đọc mô hình từ tệp nhị phân, trả về MyXGBClassificationTree**
MyXGBClassificationTree** loadModelFromMemory(const uint8_t* model_data, size_t model_size, unsigned int* num_trees) {
    MemoryBuffer buffer = {
        .buffer = model_data,
        .size = model_size,
        .position = 0
    };

    if (memory_fread(num_trees, sizeof(unsigned int), 1, &buffer) != 1) {
        ESP_LOGE("LoadModel", "Cannot read the number of trees");
        return NULL;
    }

    MyXGBClassificationTree** trees = (MyXGBClassificationTree**)malloc((*num_trees) * sizeof(MyXGBClassificationTree*));
    if (trees == NULL) {
        return NULL;
    }

    for (unsigned int i = 0; i < *num_trees; ++i) {
        TreeNode* root = loadTreeFromMemory(&buffer);
        if (root == NULL) {
            for (unsigned int j = 0; j < i; ++j) {
                freeTree(trees[j]->root);
                free(trees[j]);
            }
            free(trees);
            return NULL;
        }
        trees[i] = createMyXGBClassificationTree(root, 0, 0.0, 0.0);
    }

    return trees;
}


// Hàm xuất thông tin của cây để kiểm tra
void printTree(TreeNode* node, int depth) {
    if (node == NULL) {
        ESP_LOGI("printTree", "%*sNULL", depth * 2, ""); // In NULL nếu nút là NULL
        return;
    }

    if (node->left == NULL && node->right == NULL) {
        ESP_LOGI("printTree", "%*sLeaf: %f", depth * 2, "", node->split_point); // In giá trị lá
    } else {
        ESP_LOGI("printTree", "%*sNode: feature_id=%u, split_point=%f, gain=%f", depth * 2, "",
                 node->feature_id, node->split_point, node->gain);
        printTree(node->left, depth + 1);  // In thông tin cây con trái
        printTree(node->right, depth + 1); // In thông tin cây con phải
    }
}

// Hàm dự đoán cho cây
double predictRecursively(TreeNode* node, const double* x) {
    if (node == NULL) {
        return 0.0; // Nếu là NULL, trả về 0.0
    }

    if (node->left == NULL && node->right == NULL) {
        return node->split_point; // Trả giá trị của nút lá
    }

    if (x[node->feature_id] <= node->split_point) {
        return predictRecursively(node->left, x); // Dự đoán ở cây con trái
    } else {
        return predictRecursively(node->right, x); // Dự đoán ở cây con phải
    }
}

// Hàm dự đoán cho tất cả các mẫu
double* predict(MyXGBClassificationTree* tree, const double** x_test, unsigned int num_samples) {
    double* predictions = (double*)malloc(num_samples * sizeof(double)); // Mảng lưu dự đoán
    if (predictions == NULL) return NULL; // Kiểm tra cấp phát bộ nhớ

    for (unsigned int i = 0; i < num_samples; ++i) {
        predictions[i] = predictRecursively(tree->root, x_test[i]);
    }
    return predictions; // Trả về mảng dự đoán
}

// Cấu trúc cho bộ phân loại nhiều cây  
typedef struct MyXGBClassifier {  
    MyXGBClassificationTree** models; // Mảng chứa các cây  
    unsigned int num_models;           // Số lượng mô hình  
    double learning_rate;              // Tỷ lệ học  
    double base_score;                 // Điểm cơ sở  
} MyXGBClassifier;  

// Hàm khởi tạo bộ phân loại  
MyXGBClassifier* createMyXGBClassifier() {  
    MyXGBClassifier* classifier = (MyXGBClassifier*)malloc(sizeof(MyXGBClassifier));  
    if (classifier == NULL) {  
        ESP_LOGE("createMyXGBClassifier","Failed to allocate memory for classifier");  
        return NULL;  
    }  
    classifier->models = NULL; // Chưa có cây nào  
    classifier->num_models = 0;  
    classifier->learning_rate = 0.3;  // Tỷ lệ học mặc định  
    classifier->base_score = 0.5;     // Điểm cơ sở mặc định  
    return classifier;  
}  

// Hàm để giải phóng bộ nhớ cho bộ phân loại  
void freeMyXGBClassifier(MyXGBClassifier* classifier) {  
    if (classifier != NULL) {  
        for (unsigned int i = 0; i < classifier->num_models; ++i) {  
            freeTree(classifier->models[i]->root); // Giải phóng nút gốc trong cây  
            free(classifier->models[i]);       // Giải phóng cây  
        }  
        free(classifier->models); // Giải phóng mảng cây  
        free(classifier);         // Giải phóng bộ phân loại  
    }  
}  

// Hàm để tải các mô hình từ tệp nhị phân
void loadModels(MyXGBClassifier* classifier, const uint8_t* model_data, size_t model_size) {  
    unsigned int num_trees;
    classifier->models = loadModelFromMemory(model_data, model_size, &num_trees);

    if (classifier->models == NULL) {
        ESP_LOGE("LoadModels", "Failed to load models from memory");
        return;
    }

    classifier->num_models = num_trees;
    ESP_LOGI("LoadModels", "Successfully loaded %u trees", num_trees);
}


// Hàm dự đoán cho lớp phân loại  
double* classifierPredict(MyXGBClassifier* classifier, const double** x_test, unsigned int num_samples) {  
    double* Fm = (double*)malloc(num_samples * sizeof(double)); // Mảng lưu trữ giá trị logits  
    if (Fm == NULL) {  
        ESP_LOGE("classifierPredict","Failed to allocate memory for predictions");  
        return NULL; // Xử lý lỗi nếu không đủ bộ nhớ  
    }  

    // Khởi tạo Fm với base_score  
    for (unsigned int i = 0; i < num_samples; ++i) {  
        Fm[i] = classifier->base_score;  
    }  

    // Dự đoán từ từng cây  
    for (unsigned int m = 0; m < classifier->num_models; ++m) {  
        double* model_predictions = predict(classifier->models[m], x_test, num_samples); // Dự đoán từ cây  
        if (model_predictions == NULL) {  
            free(Fm);  
            return NULL; // Kiểm tra lỗi  
        }  
        for (unsigned int i = 0; i < num_samples; ++i) {  
            Fm[i] += classifier->learning_rate * model_predictions[i]; // Cập nhật giá trị logits  
        }  
        free(model_predictions); // Giải phóng mảng dự đoán từ cây sau khi sử dụng  
    }  

    // In giá trị logits  
    ESP_LOGI("Predict","Predictions before probability conversion:");  
    for (unsigned int i = 0; i < num_samples; ++i) {  
        printf("%.4f ", Fm[i]); // In giá trị logits  (Không biết có dùng được printf bình thường không)
    }  
    printf ("\n");

    // Chuyển đổi logits thành xác suất  
    double* probabilities = (double*)malloc(num_samples * sizeof(double));  
    if (probabilities == NULL) {  
        free(Fm);  
        return NULL; // Kiểm tra lỗi  
    }  

    ESP_LOGI("Predict","Predicted probabilities:");  
    for (unsigned int i = 0; i < num_samples; ++i) {  
        probabilities[i] = 1.0 / (1.0 + exp(-Fm[i])); // Thực hiện chuyển đổi sigmoid  
        printf("%.4f ", probabilities[i]); // In xác suất  
    }  
    printf( "\n");

    free(Fm); // Giải phóng bộ nhớ cho mảng logits  
    return probabilities; // Trả về mảng xác suất  
}
void app_main(void) {
    MyXGBClassifier* classifier = createMyXGBClassifier();
    if (classifier == NULL) {
        ESP_LOGE("Main", "Failed to create classifier");
        return;
    }

    // Sử dụng embedded model data
    size_t model_size = model_0_bst_end - model_0_bst_start;
    loadModels(classifier, model_0_bst_start, model_size);

    if (classifier->models == NULL) {
        ESP_LOGE("Main", "Failed to load models");
        freeMyXGBClassifier(classifier);
        return;
    }

    // Dữ liệu thử nghiệm
    const double* x_test[] = {
        (double[]){45.9167, 89.3638, 37.8872, 319.1341},
        (double[]){32.7403, 83.3032, 15.9857, 116.4093}
    };

    unsigned int num_samples = sizeof(x_test) / sizeof(x_test[0]);

    // Dự đoán cho dữ liệu thử nghiệm
    double* predictions = classifierPredict(classifier, x_test, num_samples);
    if (predictions != NULL) {
        free(predictions);
    }

    // Giải phóng bộ nhớ cho classifier
    freeMyXGBClassifier(classifier);
}


// void app_main(void) {  
//     printf("PREDICT V4\n");
//     MyXGBClassifier* classifier = createMyXGBClassifier(); // Create classifier  
//     if (classifier == NULL) {  
//         ESP_LOGE ("App_Main", "Failed to create classifier");
//         return; // Exit if classifier cannot be created  
//     }  

//     // Array of model filenames (only three models)  
//     const char* model_files[] = {  
//         "C:\\Users\\ASUS\\Documents\\BTL_AI\\AI_BTL\\models\\v4\\model_1.bst",  
//         "C:\\Users\\ASUS\\Documents\\BTL_AI\\AI_BTL\\models\\v4\\model_2.bst",  
//         "C:\\Users\\ASUS\\Documents\\BTL_AI\\AI_BTL\\models\\v4\\model_3.bst"  
//     };  

//     // Number of models and samples  
//     const unsigned int num_models = sizeof(model_files) / sizeof(model_files[0]);  
//     const double* x_test[] = {  
//         (double[]){45.9167, 89.3638, 37.8872, 319.1341},  
//         (double[]){32.7403, 83.3032, 15.9857, 116.4093}  
//     };  

//     unsigned int num_samples = sizeof(x_test) / sizeof(x_test[0]); // Calculate number of samples  

//     // Array to store predictions for each sample across all models  
//     int predictions_arr[num_samples][num_models];  

//     // Load models one by one and predict using each one  
//     for (unsigned int i = 0; i < num_models; ++i) {  
//         loadModels(classifier, model_files[i]); // Load each model  

//         // Generate predictions for the currently loaded model  
//         double* predictions = classifierPredict(classifier, x_test, num_samples);  
        
//         // Output predictions for the current model  
//         if (predictions != NULL) {  
//             printf("Predictions for Model %u:\n", i);  
//             for (unsigned int j = 0; j < num_samples; ++j) {  
//                 // Apply thresholding and store as integer (0 or 1)  
//                 predictions_arr[j][i] = (predictions[j] >= 0.5) ? 1 : 0;  
//                 printf("Sample %u: %d\n", j, predictions_arr[j][i]); // Print binary prediction  
//             }  
//             free(predictions); // Free memory for the predictions array  
//         } else {  
//             printf("No predictions available for Model %u.\n", i); // Handle case where predictions fail  
//         }  
//     }  

//     // Output full predictions array for all samples  
//     printf("Final Predictions Array:\n");  
//     for (unsigned int j = 0; j < num_samples; ++j) {  
//         printf("Sample %u Predictions: [", j);  
//         for (unsigned int k = 0; k < num_models; ++k) {  
//             printf("%d", predictions_arr[j][k]);  
//             if (k < num_models - 1) printf(", "); // Print comma between elements  
//         }  
//         printf("]\n");  
//     }  
//      // Output final interpreted results based on model predictions  
//     printf("Final Interpreted Results:\n");  
//     for (unsigned int j = 0; j < num_samples; ++j) {  
//         int interpreted_result = 0;  
//         int combination = predictions_arr[j][0] * 100 + predictions_arr[j][1] * 10 + predictions_arr[j][2];  

//         // Interpret results based on the specific combination  
//         if (combination == 100) {  
//             interpreted_result = 0;  
//         } else if (combination == 10) {  
//             interpreted_result = 1;  
//         } else if (combination == 1) {  
//             interpreted_result = 2;  
//         } else {  
//             interpreted_result = 0;  
//         }  

//         printf("Sample %u Interpreted Result: %d\n", j, interpreted_result);  
//     }  

//     // Clean up memory for classifier  
//     freeMyXGBClassifier(classifier);  

//     return 0; // Return code 0 to indicate success  
// }