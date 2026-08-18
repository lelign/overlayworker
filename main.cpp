#include <QGuiApplication> // 1. Меняем инклуд здесь
#include <QThread>
#include <QTimer>
#include <QImage>
#include <QPainter>
#include <QTime>
#include <QFont>
#include "overlayworker.h"
#include <QtSvg/QSvgRenderer>
#include <QDebug>

#include <QList>
#include <functional> // Для std::function

#include <QFile>
#include <QTextStream>
#include <QStringList>
#include <QResource>
#include <QDirIterator>
#include <unistd.h>

#include <cstring>

#include <algorithm> // <--- Обязательно для std::sort
 

struct CellPos {
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


CombinedConfig parseCombinedControlFile(const QString &filePath) {
    CombinedConfig cfg;
    QFile file(filePath);
    
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&file);
        // КРИТИЧЕСКОЕ ИСПРАВЛЕНИЕ ДЛЯ КИРИЛЛИЦЫ В Qt 5
        in.setCodec("UTF-8"); // Принудительно читаем как UTF-8 [4.4]
        
        cfg.analog_enable = false;
        cfg.cyan_enable = false;
        cfg.green_enable = false;
        cfg.magenta_enable = false;
        cfg.yellow_enable = false;
        while (!in.atEnd()) {
            QString line = in.readLine().trimmed();
            if (line.isEmpty() || line.startsWith("#")) continue;

            // Разбиваем строку по пробелам, запятым и табам
            QStringList tokens = line.split(QRegExp("[\\s,\\t]+"), QString::SkipEmptyParts);
            if (tokens.size() < 3) continue;

            QString key = tokens[0].toLower();
            
            // 1. ОБРАБОТКА ЛЕГКИХ КОМАНД С ДВУМЯ ПАРАМЕТРАМИ
            if (tokens.size() >= 3 && key != "label") {
                bool ok1, ok2;
                int val1 = tokens[1].toInt(&ok1);
                int val2 = tokens[2].toInt(&ok2);
                
                if (ok1 && ok2) {
                    if (key == "grid") {
                        if (val1 >= 2 && val1 <= 30 && val2 >= 2 && val2 <= 30) {
                            cfg.grid_h = val1; cfg.grid_v = val2;
                        }
                    }
                    else if (key == "analog_clock")  { cfg.analog.col = val1; cfg.analog.row = val2; cfg.analog_enable = true; }
                    else if (key == "yellow_clock")  { cfg.yellow.col = val1; cfg.yellow.row = val2; cfg.yellow_enable = true; }
                    else if (key == "magenta_clock") { cfg.magenta.col = val1; cfg.magenta.row = val2; cfg.magenta_enable = true; }
                    else if (key == "cyan_clock") { cfg.cyan.col = val1; cfg.cyan.row = val2; cfg.cyan_enable = true; }
                    else if (key == "green_clock") { cfg.green.col = val1; cfg.green.row = val2; cfg.green_enable = true; }
                }
            }
            // =================================================================
            // 2. ДИНАМИЧЕСКИЙ ПАРСИНГ НАШИХ ЯРЛЫКОВ (Команда: label col row num Title)
            // =================================================================
            else if (key == "label" && tokens.size() >= 5) {
                bool okCol, okRow, okNum;
                int readCol = tokens[1].toInt(&okCol);
                int readRow = tokens[2].toInt(&okRow);
                int readNum = tokens[3].toInt(&okNum);

                if (okCol && okRow && okNum) {
                    LabelConfigData lbl;
                    lbl.col = readCol;
                    lbl.row = readRow;
                    lbl.number = readNum;

                    // Восстанавливаем строку заголовка со всеми пробелами, отрезая первые 4 токена (label, col, row, num)
                    // И убираем возможные кавычки, если вы напишете текст в них
                    QString titleText = "";
                    for (int i = 4; i < tokens.size(); ++i) {
                        titleText += tokens[i] + " ";
                    }
                    lbl.title = titleText.trimmed().remove('\"');

                    // Добавляем распарсенный ярлык в наш динамический список
                    cfg.labels.append(lbl);
                }
            }
            // =================================================================
            // 2. ДИНАМИЧЕСКИЙ ПАРСИНГ INPUTS (Команда: input col row num Title)
            // =================================================================
            else if (key == "input" && tokens.size() >= 5) {
                bool okCol_I, okRow_I, okNum_I;
                int readCol_I = tokens[1].toInt(&okCol_I);
                int readRow_I = tokens[2].toInt(&okRow_I);
                int readNum_I = tokens[3].toInt(&okNum_I);

                if (okCol_I && okRow_I && okNum_I) {
                    InputConfigData inp;
                    inp.col = readCol_I;
                    inp.row = readRow_I;
                    inp.number = readNum_I;

                    // Восстанавливаем строку заголовка со всеми пробелами, отрезая первые 4 токена (input, col, row, num)
                    // И убираем возможные кавычки, если вы напишете текст в них
                    QString titleText = "";
                    for (int i = 4; i < tokens.size(); ++i) {
                        titleText += tokens[i] + " ";
                    }
                    inp.title = titleText.trimmed().remove('\"');

                    // Добавляем распарсенный ярлык в наш динамический список
                    cfg.staticInputs.append(inp);
                }
            }
        }
        file.close();
    } else {
        qWarning() << "Config Error: Cannot open file:" << filePath << "Reason:" << file.errorString();
    }
    
    return cfg;
}



SceneElementData calculateCellGeometry(int col, int row, int total_cols, int total_rows, int typeId) {
    // функция, которая берет параметры сетки, номер ячейки и возвращает 
    // готовые пиксельные координаты SceneElementData:

    // Физический размер одной ячейки на FullHD экране
    int cell_w = 1920 / total_cols;
    int cell_h = 1080 / total_rows;

    // Толщина серой линии сетки
    const int border_thickness = 2;

    // Защита от дурака: если в файле указали ячейку за границами текущей сетки
    int target_col = (col < total_cols) ? col : (total_cols - 1);
    int target_row = (row < total_rows) ? row : (total_rows - 1);
    if (target_col < 0) target_col = 0;
    if (target_row < 0) target_row = 0;

    // Рассчитываем точные пиксели со сдвигом внутрь ячейки
    int x = (target_col * cell_w) + border_thickness;
    int y = (target_row * cell_h) + border_thickness;
    int w = cell_w - (border_thickness * 2);
    int h = cell_h - (border_thickness * 2);

    return SceneElementData{x, y, w, h, typeId};
}

int main(int argc, char *argv[]) {
    QGuiApplication a(argc, argv);

    // 1. Создаем поток и объект воркера БЕЗ указания родителя в конструкторе
    QThread* workerThread = new QThread();
    OverlayWorker* worker = new OverlayWorker();

    // 2. СНАЧАЛА переносим воркер в асинхронный поток
    worker->moveToThread(workerThread);

    // Управляем безопасным удалением объектов при закрытии программы
    QObject::connect(workerThread, &QThread::finished, worker, &QObject::deleteLater);
    QObject::connect(workerThread, &QThread::finished, workerThread, &QThread::deleteLater);

    // 3. КРИТИЧЕСКИЙ ШАГ: Привязываем вызов инициализации к СТАРТУ потока!
    // Теперь функция initialize() выполнится строго внутри контекста нового потока.
    // Приоритет SCHED_FIFO применится именно к нужному ядру процессора.
    QObject::connect(workerThread, &QThread::started, worker, &OverlayWorker::initialize);

    // Запускаем асинхронный поток ПЛИС
    workerThread->start();

    // =========================================================================
    // СИНХРОННЫЙ АППАРАТНЫЙ ЗАПУСК ЗАСТАВКИ (СТРОГО ДО СТАРТА ЧАСОВ!)
    // =========================================================================
    // Через безопасный invokeMethod вызываем нашу изолированную заставку.
    // Флаг Qt::BlockingQueuedConnection заставит главный поток main.cpp 
    // послушно подождать ровно 4 секунды, пока на ПЛИС плавно горит приветствие!
    // Сначала парсим файл конфигурации
    CombinedConfig cfg = parseCombinedControlFile("/home/root/control.txt");
    QMetaObject::invokeMethod(worker, "runStartupGreeting", Qt::BlockingQueuedConnection);
    // =========================================================================

    // Как только 4 секунды прошли, заставка завершилась и полностью стёрлась из RAM.
    // Главный поток просыпается и запускает наш рабочий таймер обновления часов!

    // Создаем высокоточный таймер кадров
    QTimer* frameTimer = new QTimer();
    frameTimer->setTimerType(Qt::PreciseTimer);

    // Регистрируем тип данных в метасистеме Qt для безопасной межпоточной передачи
    qRegisterMetaType<QList<SceneElementData>>("QList<SceneElementData>");

        QObject::connect(frameTimer, &QTimer::timeout, [worker]() {
            // Защита очереди (Drop-Frame)
            if (worker->isBusy()) {
                return; 
            }

            QElapsedTimer totalTimer;
            totalTimer.start();

            // 1. Парсим один единственный файл конфигурации
            CombinedConfig cfg = parseCombinedControlFile("/home/root/control.txt");

            // Математика сетки: ячеек всегда на 1 больше, чем линий
            int total_cols = cfg.grid_v + 1;
            int total_rows = cfg.grid_h + 1;

            // 2. Формируем список метаданных для отправки воркеру
            QList<SceneElementData> sceneData;
            //sceneData.reserve(10 + cfg.labels.size()); // Резервируем память под часы + массив ярлыков
            sceneData.reserve(10 + cfg.labels.size() + cfg.staticInputs.size()); // Резервируем память под часы + массив labels + массив inputs

            // Слои формируются на лету:
            // Индекс 0: Сетка (передаем количество линий из конфига)
            sceneData.append(SceneElementData{0, 0, cfg.grid_h, cfg.grid_v, 0});
            
            // Автоматически рассчитываем пиксели для каждого элемента по его ячейке!
            // Больше никаких ручных пикселей и никаких 5-х параметров (typeId зашит жестко)
            if(cfg.yellow_enable){
                sceneData.append(calculateCellGeometry(cfg.yellow.col,  cfg.yellow.row,  total_cols, total_rows, 1)); // Желтые часы
            };
            if(cfg.analog_enable){
                sceneData.append(calculateCellGeometry(cfg.analog.col,  cfg.analog.row,  total_cols, total_rows, 2)); // Аналоговые часы
            };
            if(cfg.green_enable){
                sceneData.append(calculateCellGeometry(cfg.green.col, cfg.green.row, total_cols, total_rows, 3)); //  Зеленые 1
            };
            if(cfg.magenta_enable){
                sceneData.append(calculateCellGeometry(cfg.magenta.col, cfg.magenta.row, total_cols, total_rows, 4)); // Малиновые часы
            };
            if(cfg.cyan_enable){
                sceneData.append(calculateCellGeometry(cfg.cyan.col, cfg.cyan.row, total_cols, total_rows, 5));  // голубые
            };
                
            
            
            
            // ... (Ваш код чтения control.txt и добавления сетки и часов) ...
            // sceneData.append(calculateCellGeometry(cfg.yellow.col,  cfg.yellow.row,  total_cols, total_rows, 1));
            // ...
            // Статические зеленые часы (оставляем на старых фиксированных местах, если нужно)
            //sceneData.append(SceneElementData{780, 570, 1000, 210, 3}); // Зеленые 1
            //sceneData.append(SceneElementData{740, 800, 1000, 210, 5}); // Зеленые 2

            // =================================================================
            // АВТОМАТИЧЕСКИЙ СЛИТНЫЙ ОБХОД ВСЕХ ЯРЛЫКОВ ИЗ ФАЙЛА
            // =================================================================
            for (const LabelConfigData &lbl : cfg.labels) {
                // Вычисляем пиксельные координаты ячейки [col, row] для typeId == 6
                SceneElementData labelElement = calculateCellGeometry(lbl.col, lbl.row, total_cols, total_rows, 6);
                
                // Насыщаем элемент уникальным номером и текстом заголовка
                labelElement.labelNum = lbl.number;
                labelElement.labelTitle = lbl.title;
                
                sceneData.append(labelElement);
            }

            // =================================================================
            // АВТОМАТИЧЕСКИЙ СЛИТНЫЙ ОБХОД ВСЕХ INPUTS ИЗ ФАЙЛА
            // =================================================================        
            for (const InputConfigData &inp : cfg.staticInputs) {
                // Вычисляем пиксельные координаты ячейки [col, row] для typeId == 6
                SceneElementData inputElement = calculateCellGeometry(inp.col, inp.row, total_cols, total_rows, 8);
                
                // Насыщаем элемент уникальным номером и текстом заголовка
                inputElement.inputStaticNumber = inp.number;
                inputElement.inputStaticTitle = inp.title;
                qDebug() << "inputElement.inputStaticNumber" << inputElement.inputStaticNumber
                        << "inputElement.inputStaticTitle" << inputElement.inputStaticTitle;
                sceneData.append(inputElement);
            }
            
            // =================================================================
            // ЗАПУСК ПОЛНОЭКРАННОГО ПРИВЕТСТВИЯ НА 4 СЕКУНДЫ С АВТО-УДАЛЕНИЕМ
            // =================================================================
            static QElapsedTimer globalLifeTimer;
            static bool isLifeTimerStarted = false;
            if (!isLifeTimerStarted) {
                globalLifeTimer.start();
                isLifeTimerStarted = true;
            }

            // Удерживаем элемент заставки в списке в первые 4.1 секунды работы программы
            if (globalLifeTimer.elapsed() < 4100) {
                // Размещение 0,0, ширина 1920, высота 1080, typeId = 7
                sceneData.append(SceneElementData{0, 0, 1920, 1080, 7});
            }
            // =================================================================

            // Сортируем Z-Order слоев
            std::sort(sceneData.begin(), sceneData.end(), [](const SceneElementData &a, const SceneElementData &b) {
                return a.typeId < b.typeId;
            });

            // Асинхронно отправляем отсортированную сцену воркеру
            QMetaObject::invokeMethod(worker, "process_and_write_full_scene", 
                                    Qt::QueuedConnection, 
                                    Q_ARG(QList<SceneElementData>, sceneData));
        }
    );


    frameTimer->start(20); // Стабильные 20 FPS (каждые 50 мс)
    return a.exec();
}



