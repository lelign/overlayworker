#ifndef OVERLAYWORKER_H
#define OVERLAYWORKER_H

#include <QObject>
#include <QImage>
#include <QList>

// #include <QMutex>
// #include <QWaitCondition>



// Чтобы передавать параметры элементов через очереди Qt асинхронно, 
// нам нужен простой плоский тип данных, который легко копируется. 
// Создадим структуру SceneElementData
// 1. Переносим объявление структуры сюда, чтобы компилятор знал этот тип во всем проекте
struct SceneElementData {
    int offset_x;
    int offset_y;
    int width;
    int height;
    // typeId = 0 - сетка (static), 
    //          1 - желтые часы (zzz), 
    //          2 - аналоговые часы, 
    //          3 - зеленые часы
    //          4 - magenta часы
    //          5 - cyan часы (z)
    //          6 - labels (static)
    //          7 - greeting (start/reboot only 4 sec)
    //          8 - inputs (static)
    int typeId;
    // поля для статических лейблов (по умолчанию пустые)
    int labelNum = 0;
    QString labelTitle = "";
    // поля для статических inputs (по умолчанию пустые)
    int inputStaticNumber = 0;
    QString inputStaticTitle = "";
};

struct CellPos { // перенес из main
    int col = 0;
    int row = 0;
};

struct LabelConfigData {
    int col = 0;
    int row = 0;
    int number = 0;
    QString title = "";
};

struct InputConfigData {
    int col = 0;
    int row = 0;
    int number = 0;
    QString title = "";
};

struct CombinedConfig {
    int grid_h = 2; // Дефолтные значения
    int grid_v = 2;
    bool analog_enable;
    CellPos analog;
    bool yellow_enable;
    CellPos yellow;
    bool magenta_enable;
    CellPos magenta;
    bool cyan_enable;
    CellPos cyan;
    bool green_enable;
    CellPos green;    
    QList<LabelConfigData> labels; // <-- Динамический список для любого количества ярлыков (хоть 30 штук!)
    QList<InputConfigData> staticInputs; // <-- Динамический список для любого количества inputs
};

#pragma pack(push, 1)
// pragma
// По умолчанию компиляторы (особенно под ARM) часто выравнивают структуры по границе 4 или 8 байт, 
// чтобы процессору было легче их читать.
// Без упаковки структура из 6 байт (MacroPixel) заняла бы в памяти 8 байт (2 байта были бы пустым мусором).
// С вашей упаковкой структура займет ровно 6 байт — байт в байт.
// Это критически важно, так как вы, скорее всего, будете накладывать этот указатель на 
// сырой буфер пикселей (DMA или файл) или читать данные напрямую через sizeof(MacroPixel). 
// Любое лишнее смещение сломало бы картинку.

    struct MacroPixel {
        unsigned char cb, y0, alpha0, cr, y1, alpha1;
    };
#pragma pack(pop)

class OverlayWorker : public QObject {
    Q_OBJECT
public:
    // QMutex m_syncMutex;
    // QWaitCondition m_syncCondition;
    QList<SceneElementData> m_nextSceneData;
    bool m_hasNewData = false;

    // Слот process_and_write_full_scene больше НЕ нужен как слот Qt! 
    // Мы перенесем его в бесконечный рабочий цикл потока.
    // void startWorkerLoop(); 
    explicit OverlayWorker(QObject *parent = nullptr);
    ~OverlayWorker();

    bool initialize();
    // аведем воркеру простой флаг занятости m_is_busy. Если воркер занят, 
    // главный поток просто не будет спамить очередь.
    bool isBusy() const { return m_is_busy; }
    // Добавляем макрос Q_INVOKABLE, чтобы Qt Meta-Object Compiler 
    // зарегистрировал эту функцию и её можно было вызывать через invokeMethod!
    // Q_INVOKABLE void runStartupGreeting(); 
    

public slots:
    // Функция векторной ARM NEON конвертации монолитного sharedCanvas 1920x1080 в YUV
    // void write_full_frame(const QImage &img);
    
    // Функция выполняет аппаратное переключение экранов (вызывается в конце кадра)
    void flip_buffer();

    // Новый асинхронный слот, который будет собирать всю сцену в потоке воркера
    void process_and_write_full_scene(const QList<SceneElementData> &sceneData);
    void convert_and_write_rect(const QImage &sub_img, int target_x, int target_y);
    // Метод теперь принимает структуру по ссылке (или по значению)
    // Q_INVOKABLE void runStartupGreeting(const CombinedConfig &cfg);
    void runStartupGreeting(const CombinedConfig &cfg); // Q_INVOKABLE в случае подключения через слот не нужен

private:
    int m_fd = -1;
    unsigned char* m_mmap_base = nullptr;
    unsigned int m_current_display_idx = 0;
    
    const int FRAME_WIDTH = 1920;
    const int FRAME_HEIGHT = 1080;
    const int STRIDE = 1920 * 3;
    const size_t FRAME_SIZE = 6221824;
    const size_t TOTAL_MAP_SIZE = 6221824 * 2;
    
    MacroPixel m_bg_pixel;   
    // inside overlayworker.h в секции private:
    QImage generateFullScreenGrid(int step_x, int step_y);
    QImage generateYellowClock(int clock_width, int clock_height);
    QImage generateTimeImageGreen(int clock_width, int clock_height);
    QImage generateTimeImageMagentaAlpha(int clock_width, int clock_height);
    QImage generateTimeImageCyan(int clock_width, int clock_height);
    QImage generateLabel(int label_width, int label_height, int labelNumber, QString labelTitle);
    QImage generateInputStatic(int inputStatic_width, int inputStatic_height, int inputStaticNumber, QString inputStaticTitle);
    QImage generateYellowClockOnlyMS(int clock_width, int clock_height);

    // аведем воркеру простой флаг занятости m_is_busy. Если воркер занят, 
    // главный поток просто не будет спамить очередь.
    bool m_is_busy = false;
};

#endif // OVERLAYWORKER_H
