#include "overlayworker.h"
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <QDebug>
#include <cerrno>
#include <algorithm>

#include <arm_neon.h>

#include <pthread.h>
#include <sched.h>

#include <QElapsedTimer> // <--- Добавьте эту строчку

#include <QImage>
#include <QPainter>
#include <QTime>
#include <QFont>
#include <QtSvg/QSvgRenderer>

#include <cstring>

#include <cmath> // for greeting

//#include <sys/mman.h> already exist

#include <QThread>


#define OVERLAY_MAGIC 'm'
#define OVERLAY_IOCTL_FLIP _IOW(OVERLAY_MAGIC, 14, int)

OverlayWorker::OverlayWorker(QObject *parent) : QObject(parent) {
    m_bg_pixel = {128, 0, 255, 128, 0, 255}; // Наш чистый YUV черный фон
}

OverlayWorker::~OverlayWorker() {
    if (m_mmap_base && m_mmap_base != MAP_FAILED) {
        munmap(m_mmap_base, TOTAL_MAP_SIZE);
    }
    if (m_fd >= 0) {
        close(m_fd);
    }
    qDebug() << "OverlayWorker destroyed, resources released.";
}

bool OverlayWorker::initialize() {
    // ------------------------------------------------------------------
    // SET POSIX REAL-TIME SCHEDULER PRIORITY (SCHED_FIFO)
    // ------------------------------------------------------------------
    pthread_t this_thread = pthread_self();
    struct sched_param params;
    
    // Choose a high real-time priority (e.g., 80 out of 99 max)
    // This places your worker above standard Linux tasks and logging daemons
    params.sched_priority = 80; 

    int result = pthread_setschedparam(this_thread, SCHED_FIFO, &params);
    if (result == 0) {
        qInfo() << "Success: OverlayWorker thread escalated to SCHED_FIFO (RT Priority 80)";
    } else {
        qWarning() << "Warning: Failed to set SCHED_FIFO priority! Error code:" << result;
        qWarning() << "Note: Make sure to execute the binary as 'root' or with sudo privileges.";
    }
    

    m_fd = open("/dev/mtv-overlay", O_RDWR);
    if (m_fd < 0) {
        qCritical() << "Worker error: Cannot open /dev/mtv-overlay! errno:" << errno;
        return false;
    }

    m_mmap_base = (unsigned char*)mmap(NULL, TOTAL_MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, m_fd, 0);
    if (m_mmap_base == MAP_FAILED) {
        qCritical() << "Worker error: Mmap failed! errno:" << errno;
        close(m_fd);
        m_fd = -1;
        return false;
    }

    // Первичная очистка обоих буферов при старте
    for (int y = 0; y < FRAME_HEIGHT; ++y) {
        std::fill_n((MacroPixel*)(m_mmap_base + (y * STRIDE)), FRAME_WIDTH / 2, m_bg_pixel);
        std::fill_n((MacroPixel*)(m_mmap_base + FRAME_SIZE + (y * STRIDE)), FRAME_WIDTH / 2, m_bg_pixel);
    }
    
#if defined(__arm__) || defined(__aarch64__)
    asm volatile("dsb sy" : : : "memory");
#endif

    qDebug() << "OverlayWorker successfully initialized in hardware.";
    return true;
}

void OverlayWorker::convert_and_write_rect(const QImage &sub_img, int target_x, int target_y) {
    if (sub_img.isNull() || m_mmap_base == nullptr) return;

    QImage workingImg = sub_img.convertToFormat(QImage::Format_RGBA8888);

    int img_w = workingImg.width();
    int img_h = workingImg.height();

    int nextWriteIndex = (m_current_display_idx == 0) ? 1 : 0;
    unsigned char* active_fb_ptr = m_mmap_base + (nextWriteIndex * FRAME_SIZE);

    int16x4_t v_coeff_y_r  = vdup_n_s16(77);
    int16x4_t v_coeff_y_g  = vdup_n_s16(150);
    int16x4_t v_coeff_y_b  = vdup_n_s16(29);
    int16x4_t v_coeff_cb_r = vdup_n_s16(-43);
    int16x4_t v_coeff_cb_g = vdup_n_s16(-85);
    int16x4_t v_coeff_cb_b = vdup_n_s16(128);
    int16x4_t v_coeff_cr_r = vdup_n_s16(128);
    int16x4_t v_coeff_cr_g = vdup_n_s16(-107);
    int16x4_t v_coeff_cr_b = vdup_n_s16(-21);
    int16x8_t v_128_vec = vdupq_n_s16(128);

    // FIXED: Полностью убрали округление target_x & ~1! 
    // Теперь пиксели ложатся строго в те координаты, которые переданы из main!
    int start_x = target_x; 

    for (int src_y = 0; src_y < img_h; ++src_y) {
        int dst_y = target_y + src_y;
        if (dst_y < 0 || dst_y >= 1080) continue; 

        const unsigned char* rgba_row = workingImg.constScanLine(src_y);
        unsigned char* dma_row_ptr = active_fb_ptr + (dst_y * 1920 * 3);
        MacroPixel* dma_row = reinterpret_cast<MacroPixel*>(dma_row_ptr);

        // Бежим по ширине картинки шагами по 8 пикселей
        for (int src_x = 0; src_x < img_w; src_x += 8) {
            // Вычисляем точный индекс макропикселя в строке ПЛИС без сдвигов влево!
            int current_macro_idx = (start_x + src_x) / 2;

            if (current_macro_idx + 3 >= (1920 / 2)) break;

            // БЫСТРАЯ МАСКА: Если блок из 8 пикселей полностью прозрачный, пропускаем его!
            // Это мгновенно защищает вертикальные линии сетки от затирания "воздухом" часов!
            if ((*reinterpret_cast<const unsigned int*>(&rgba_row[src_x * 4 + 0]) == 0) &&
                (*reinterpret_cast<const unsigned int*>(&rgba_row[src_x * 4 + 8]) == 0) &&
                (*reinterpret_cast<const unsigned int*>(&rgba_row[src_x * 4 + 16]) == 0) &&
                (*reinterpret_cast<const unsigned int*>(&rgba_row[src_x * 4 + 24]) == 0)) {
                continue; 
            }

            uint8x16x4_t rgba_pixels = vld4q_u8(&rgba_row[src_x * 4]);

            uint8x8_t r_8 = vget_low_u8(rgba_pixels.val[0]);
            uint8x8_t g_8 = vget_low_u8(rgba_pixels.val[1]);
            uint8x8_t b_8 = vget_low_u8(rgba_pixels.val[2]);
            uint8x8_t v_alpha = vget_low_u8(rgba_pixels.val[3]);

            int16x8_t r_16 = reinterpret_cast<int16x8_t>(vmovl_u8(r_8));
            int16x8_t g_16 = reinterpret_cast<int16x8_t>(vmovl_u8(g_8));
            int16x8_t b_16 = reinterpret_cast<int16x8_t>(vmovl_u8(b_8));

            int16x4_t r_l = vget_low_s16(r_16);   int16x4_t r_h = vget_high_s16(r_16);
            int16x4_t g_l = vget_low_s16(g_16);   int16x4_t g_h = vget_high_s16(g_16);
            int16x4_t b_l = vget_low_s16(b_16);   int16x4_t b_h = vget_high_s16(b_16);

            int32x4_t y_l = vmull_s16(r_l, v_coeff_y_r);
            y_l = vmlal_s16(y_l, g_l, v_coeff_y_g);
            y_l = vmlal_s16(y_l, b_l, v_coeff_y_b);

            int32x4_t y_h = vmull_s16(r_h, v_coeff_y_r);
            y_h = vmlal_s16(y_h, g_h, v_coeff_y_g);
            y_h = vmlal_s16(y_h, b_h, v_coeff_y_b);

            int16x4_t y_l_16 = vshrn_n_s32(y_l, 8);
            int16x4_t y_h_16 = vshrn_n_s32(y_h, 8);
            uint8x8_t v_y = vqmovun_s16(vcombine_s16(y_l_16, y_h_16));

            int32x4_t cb_l = vmull_s16(r_l, v_coeff_cb_r);
            cb_l = vmlal_s16(cb_l, g_l, v_coeff_cb_g);
            cb_l = vmlal_s16(cb_l, b_l, v_coeff_cb_b);

            int32x4_t cb_h = vmull_s16(r_h, v_coeff_cb_r);
            cb_h = vmlal_s16(cb_h, g_h, v_coeff_cb_g);
            cb_h = vmlal_s16(cb_h, b_h, v_coeff_cb_b);

            int16x8_t cb_16 = vcombine_s16(vshrn_n_s32(cb_l, 8), vshrn_n_s32(cb_h, 8));
            uint8x8_t v_cb = vqmovun_s16(vaddq_s16(cb_16, v_128_vec));

            int32x4_t cr_l = vmull_s16(r_l, v_coeff_cr_r);
            cr_l = vmlal_s16(cr_l, g_l, v_coeff_cr_g);
            cr_l = vmlal_s16(cr_l, b_l, v_coeff_cr_b);

            int32x4_t cr_h = vmull_s16(r_h, v_coeff_cr_r);
            cr_h = vmlal_s16(cr_h, g_h, v_coeff_cr_g);
            cr_h = vmlal_s16(cr_h, b_h, v_coeff_cr_b);

            int16x8_t cr_16 = vcombine_s16(vshrn_n_s32(cr_l, 8), vshrn_n_s32(cr_h, 8));
            uint8x8_t v_cr = vqmovun_s16(vaddq_s16(cr_16, v_128_vec));

            uint8x8_t v_yuv_min_limit = vdup_n_u8(16);  
            uint8x8_t v_yuv_max_limit = vdup_n_u8(235); 
            v_cb = vmax_u8(v_cb, v_yuv_min_limit); v_cb = vmin_u8(v_cb, v_yuv_max_limit); 
            v_cr = vmax_u8(v_cr, v_yuv_min_limit); v_cr = vmin_u8(v_cr, v_yuv_max_limit);

            uint8x8x2_t cb_split = vuzp_u8(v_cb, v_cb);
            uint8x8_t v_cb_avg = vhadd_u8(cb_split.val[0], cb_split.val[1]);

            uint8x8x2_t cr_split = vuzp_u8(v_cr, v_cr);
            uint8x8_t v_cr_avg = vhadd_u8(cr_split.val[0], cr_split.val[1]);

            // ==============================================================
            // АБСОЛЮТНАЯ ЗАЩИТА ВЕРТИКАЛЬНОЙ СЕТКИ (HARDWARE SYMMETRIC MASK)
            // ==============================================================
            // Вычисляем точную физическую координату X на FullHD экране (1920) 
            // для каждого из 4-х макропикселей в текущей пачке NEON
            int screen_x0 = start_x + src_x + 0;
            int screen_x2 = start_x + src_x + 2;
            int screen_x4 = start_x + src_x + 4;
            int screen_x6 = start_x + src_x + 6;

            // Граница, где начинается вертикальная линия сетки соседа:
            int grid_right_edge = start_x + img_w;

            // Макропиксель 0 (Обслуживает пиксели screen_x0 и screen_x0 + 1)
            // Пишем в DDR только если пиксели видимы И они строго не доходят до линии сетки соседа!
            if ((vget_lane_u8(v_alpha, 0) > 0 || vget_lane_u8(v_alpha, 1) > 0) && (screen_x0 + 1 < grid_right_edge)) {
                dma_row[current_macro_idx + 0] = MacroPixel {
                    vget_lane_u8(v_cb_avg, 0), vget_lane_u8(v_y, 0), vget_lane_u8(v_alpha, 0),
                    vget_lane_u8(v_cr_avg, 0), vget_lane_u8(v_y, 1), vget_lane_u8(v_alpha, 1)
                };
            }

            // Макропиксель 1 (Обслуживает пиксели screen_x2 и screen_x2 + 1)
            if ((vget_lane_u8(v_alpha, 2) > 0 || vget_lane_u8(v_alpha, 3) > 0) && (screen_x2 + 1 < grid_right_edge)) {
                dma_row[current_macro_idx + 1] = MacroPixel {
                    vget_lane_u8(v_cb_avg, 1), vget_lane_u8(v_y, 2), vget_lane_u8(v_alpha, 2),
                    vget_lane_u8(v_cr_avg, 1), vget_lane_u8(v_y, 3), vget_lane_u8(v_alpha, 3)
                };
            }

            // Макропиксель 2 (Обслуживает пиксели screen_x4 и screen_x4 + 1)
            if ((vget_lane_u8(v_alpha, 4) > 0 || vget_lane_u8(v_alpha, 5) > 0) && (screen_x4 + 1 < grid_right_edge)) {
                dma_row[current_macro_idx + 2] = MacroPixel {
                    vget_lane_u8(v_cb_avg, 2), vget_lane_u8(v_y, 4), vget_lane_u8(v_alpha, 4),
                    vget_lane_u8(v_cr_avg, 2), vget_lane_u8(v_y, 5), vget_lane_u8(v_alpha, 5)
                };
            }

            // Макропиксель 3 (Обслуживает пиксели screen_x6 и screen_x6 + 1)
            if ((vget_lane_u8(v_alpha, 6) > 0 || vget_lane_u8(v_alpha, 7) > 0) && (screen_x6 + 1 < grid_right_edge)) {
                dma_row[current_macro_idx + 3] = MacroPixel {
                    vget_lane_u8(v_cb_avg, 3), vget_lane_u8(v_y, 6), vget_lane_u8(v_alpha, 6),
                    vget_lane_u8(v_cr_avg, 3), vget_lane_u8(v_y, 7), vget_lane_u8(v_alpha, 7)
                };
            }
        }
    }
}

void OverlayWorker::flip_buffer() {
    if (m_fd < 0) return;

    int nextWriteIndex = (m_current_display_idx == 0) ? 1 : 0;

    #if defined(__arm__) || defined(__aarch64__)
        asm volatile("dsb sy" : : : "memory");
    #endif

    // Вызываем переключение ОДИН раз, когда оба изображения гарантированно на месте
    int result = ioctl(m_fd, OVERLAY_IOCTL_FLIP, &nextWriteIndex);
    if (result >= 0) {
        m_current_display_idx = nextWriteIndex;
    } else {
        qCritical() << "Worker IOCTL FLIP failed!";
    }
}

#include <cmath> // Убедитесь, что этот инклюд есть вверху для функции std::sin

QImage OverlayWorker::generateGreeting(int canvas_width, int canvas_height) {
    QImage img(canvas_width, canvas_height, QImage::Format_RGBA8888);
    img.fill(Qt::transparent); 

    static QElapsedTimer greetingTimer;
    static bool isTimerStarted = false;
    
    if (!isTimerStarted) {
        greetingTimer.start();
        isTimerStarted = true;
    }

    qint64 elapsed = greetingTimer.elapsed();

    // FIXED: Теперь заставка длится ровно 4 секунды (4000 мс)
    if (elapsed > 4000) {
        return img; 
    }

    // =================================================================
    // СВЕРХПЛАВНАЯ МАТЕМАТИКА SINE-SMOOTHING (0 мс на CPU)
    // =================================================================
    // 1. Нормализуем время: переводим 0..4000 мс в диапазон от 0.0 до 1.0
    double progress = elapsed / 4000.0;

    // 2. Используем полуволну синуса (от 0 до Pi).
    // На отрезке progress [0..1] функция sin(progress * M_PI) выдаст идеальную 
    // колоколообразную кривую: начнется в 0.0, плавно взлетит до 1.0 ровно 
    // посередине (на 2-й секунде) и сверхплавно опустится в 0.0 к концу.
    double smoothFactor = std::sin(progress * M_PI);

    // 3. Переводим коэффициент плавности в значение прозрачности Alpha (0..255)
    int alpha = static_cast<int>(smoothFactor * 255.0);

    if (alpha < 0) alpha = 0;
    if (alpha > 255) alpha = 255;
    // =================================================================

    QPainter painter(&img);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    // 1. ПЛАВНОЕ ЗАТЕМНЕНИЕ ВСЕГО ЭКРАНА СИНУСОИДАЛЬНОЙ ПОДЛОЖКОЙ
    int bgAlpha = static_cast<int>(alpha * 0.75); // Максимальное затемнение фона — около 190
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, bgAlpha)); 
    painter.drawRect(img.rect());

    // 2. ДИНАМИЧЕСКИЙ ОРАНЖЕВЫЙ ЦВЕТ С ГЛУБОКИМ ГРАДИЕНТОМ НАСЫЩЕНИЯ
    // Благодаря smoothFactor, цвет рождается из полной темноты (0,0,0), 
    // нарастает до глубокого терракотового и в пике раскрывается в сочный оранжевый (255, 120, 0)
    int red = static_cast<int>(smoothFactor * 255.0);   
    int green = static_cast<int>(smoothFactor * 120.0); 
    int blue = 0;                                        
    
    QColor dynamicOrange(red, green, blue, alpha);

    // 3. ОГРОМНЫЙ ШРИФТ ДЛЯ ЗАГОЛОВКА
    int bigFontSize = canvas_width / 18; // Сделали шрифт еще чуть крупнее и солиднее
    QFont font("Arial", bigFontSize, QFont::Bold);
    font.setKerning(false);
    painter.setFont(font);

    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    painter.setPen(dynamicOrange);

    // Рисуем заголовок "Greeting" идеально по центру FullHD холста
    painter.drawText(img.rect(), Qt::AlignCenter, "PROFITT PBX-MTV-5161");
    
    painter.end();

    return img;
}

// полностью измененный и оптимизированный код функции, который защитит пропорции и автоматически 
// отцентрирует часы внутри любого прямоугольника:
QImage renderSvgToImage_no_parse_arrows(int height, int width) {
    // 1. ОПРЕДЕЛЯЕМ ПРОПОРЦИИ И ЦЕНТРИРОВАНИЕ КРУГА ВНУТРИ ПРЯМОУГОЛЬНИКА
    // Текстовая плашка съедает 50 пикселей снизу. Чистая доступная высота под круг:
    int available_height = height - 50;
    if (available_height < 10) available_height = 10; // Защита от нулевой высоты

    // Часы должны быть круглыми, поэтому берем минимальную сторону
    int clock_size = (width < available_height) ? width : available_height;

    // Считаем отступы (offsets), чтобы отцентрировать круглый циферблат внутри холста
    int offset_x = (width - clock_size) / 2;
    int offset_y = (available_height - clock_size) / 2;

    // Итоговая высота холста QImage (высота ячейки)
    int canvas_height = height; 
    
    static QImage staticImageCache;
    static bool isCacheInitialized = false;
    static int old_w = 0;
    static int old_h = 0;

    // Кэшируем подложку циферблата только при старте или смене разрешения
    if (!isCacheInitialized || old_w != width || old_h != height) {
        staticImageCache = QImage(width, canvas_height, QImage::Format_RGBA8888);
        staticImageCache.fill(Qt::transparent); 

        QSvgRenderer face(QString(":/image/clock/face.svg"));
        if (!face.isValid()) {
            qCritical() << "Failed to load SVG file:/image/clock/face.svg";
            return staticImageCache;
        }

        QPainter cachePainter(&staticImageCache);
        cachePainter.setRenderHint(QPainter::Antialiasing, true);
        cachePainter.setRenderHint(QPainter::TextAntialiasing, true);

        // Рендерим круглый циферблат строго по центру выделенной зоны
        QRectF faceRect(offset_x, offset_y, clock_size, clock_size);
        face.render(&cachePainter, faceRect); 

        // Рисуем текст строго под круглым циферблатом, центрируя по ширине всего холста
        // Текст пишется в прямоугольнике, который начинается сразу под зоной круга
        QFont font("Arial", clock_size / 15 + 8, QFont::Bold); // Масштабируем шрифт под размер часов
        cachePainter.setFont(font);
        cachePainter.setPen(Qt::white); 

         // FIXED: Привязываем прямоугольник текста строго под нижний край круга!
        // Начало по Y = отступ круга + диаметр круга
        int text_y_start = offset_y + clock_size;
        
        // QRect textRect(0, available_height, width, 50);
        // cachePainter.drawText(textRect, Qt::AlignCenter, "Arria 10");

        // Рисуем текст в прямоугольнике шириной во весь холст, 
        // который начинается сразу под часами и имеет высоту 50 пикселей
        QRect textRect(0, text_y_start, width, 50);
        cachePainter.drawText(textRect, Qt::AlignCenter, "Arria 10");
        
        cachePainter.end();
        old_w = width;
        old_h = height;
        isCacheInitialized = true;
        qDebug() << "SVG Background successfully cached in RAM with safe proportions!";
    }

    // Мгновенно копируем независимую подложку из ОЗУ
    QImage img(width, canvas_height, QImage::Format_RGBA8888);
    memcpy(img.bits(), staticImageCache.constBits(), staticImageCache.sizeInBytes());

    QPainter painter(&img);
    painter.setRenderHint(QPainter::Antialiasing, true); // Идеальное сглаживание стрелок

    QTime time = QTime::currentTime();
    double secondsAngle = (time.second() * 6.0) + (time.msec() * 0.006);
    double minutesAngle = (time.minute() * 6.0) + (time.second() * 0.1);
    double hoursAngle = ((time.hour() % 12) * 30.0) + (time.minute() * 0.5);

    // 2. СДВИГАЕМ ЦЕНТР ВРАЩЕНИЯ СТРЕЛОК С УЧЕТОМ СДВИГОВ ЦЕНТРИРОВАНИЯ
    int center_x = offset_x + (clock_size / 2);
    int center_y = offset_y + (clock_size / 2);
    painter.translate(center_x, center_y);

    // Масштабируем радиус стрелок строго от реального размера круглого циферблата
    double radius = clock_size / 2.0;

    // Адаптируем толщину хвостов и стрелок под размер часов (чтобы на маленьких часах они не были огромными)
    double base_scale = clock_size / 1000.0;
    if (base_scale < 0.2) base_scale = 0.2;

    // 1. ЧАСОВАЯ СТРЕЛКА
    painter.save();
    painter.rotate(hoursAngle);
    painter.setPen(Qt::NoPen);
    painter.setBrush(Qt::black);
    QPolygonF hourHand;
    hourHand << QPointF(-10 * base_scale,  20 * base_scale)   
             << QPointF(0,                30 * base_scale)   
             << QPointF(10 * base_scale,   20 * base_scale)   
             << QPointF(6 * base_scale,   -radius * 0.5) 
             << QPointF(0,                -radius * 0.6) 
             << QPointF(-6 * base_scale,  -radius * 0.5); 
    painter.drawPolygon(hourHand);
    painter.restore();

    // 2. МИНУТНАЯ СТРЕЛКА
    painter.save();
    painter.rotate(minutesAngle);
    painter.setPen(Qt::NoPen);
    painter.setBrush(Qt::black);
    QPolygonF minuteHand;
    minuteHand << QPointF(-7 * base_scale,   30 * base_scale)
               << QPointF(0,                 40 * base_scale)
               << QPointF(7 * base_scale,   30 * base_scale)
               << QPointF(4 * base_scale,   -radius * 0.75)
               << QPointF(0,                 -radius * 0.85)
               << QPointF(-4 * base_scale,  -radius * 0.75);
    painter.drawPolygon(minuteHand);
    painter.restore();

    // 3. СЕКУНДНАЯ СТРЕЛКА
    painter.save();
    painter.rotate(secondsAngle);
    painter.setPen(Qt::NoPen);
    painter.setBrush(Qt::red);
    QPolygonF secondHand;
    secondHand << QPointF(-3 * base_scale,   40 * base_scale)
               << QPointF(3 * base_scale,    40 * base_scale)
               << QPointF(1.5 * base_scale, -radius * 0.9)
               << QPointF(-1.5 * base_scale,-radius * 0.9);
    painter.drawPolygon(secondHand);
    painter.restore();

    // Центральная ось
    painter.setPen(Qt::NoPen); 
    painter.setBrush(Qt::red);
    int center_dot_size = static_cast<int>(8 * base_scale);
    if (center_dot_size < 3) center_dot_size = 3;
    painter.drawEllipse(QPoint(0, 0), center_dot_size, center_dot_size);

    painter.end();
    return img;
}

QImage OverlayWorker::generateTimeImageGreen(int clock_width, int clock_height) {
    // 1. Создаем локальный холст в формате RGBA8888
    QImage img(clock_width, clock_height, QImage::Format_RGBA8888);
    img.fill(Qt::transparent); // Вся ячейка изначально полностью прозрачна

    // =========================================================================
    // ЖЕСТКИЙ ЗАЩИТНЫЙ МАКЕТ С ОТСТУПОМ ОТ ВЕРТИКАЛЬНОЙ СЕТКИ СЛЕВА И СПРАВА
    // =========================================================================
    // Отрезаем по 5 пикселей по бокам. Мертвая зона гарантирует Alpha = 0 на краях,
    // и наш конвертер никогда не заденет серо-белые рамки ячеек соседа!
    QRect safeRect(5, 0, clock_width - 10, clock_height);
    // =========================================================================

    // =========================================================================
    // АДАПТИВНЫЙ РАСЧЕТ РАЗМЕРА ШРИФТА (ДЛЯ СТРОКИ ИЗ 8 СИМВОЛОВ hh:mm:ss)
    // =========================================================================
    // Итоговая строка таймера вида "00:00:00" теперь содержит ровно 8 символов.
    // Задаем расчетную длину 9.2 символа для мягких отступов по бокам.
    // У моноширинного шрифта (Monospace) пропорция ширины к высоте равна 0.6.
    int fontSizeByWidth = static_cast<int>(clock_width / (9.2 * 0.6));
    int fontSizeByHeight = static_cast<int>(clock_height * 0.82); // 18% запас по вертикали
    
    // Выбираем минимальный размер, чтобы текст гарантированно влез в рамки ячейки
    int optimalFontSize = (fontSizeByWidth < fontSizeByHeight) ? fontSizeByWidth : fontSizeByHeight;
    if (optimalFontSize < 8) optimalFontSize = 8; // Защита от нулевого шрифта
    // =========================================================================

    QPainter painter(&img);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    // Конфигурируем адаптивный моноширинный шрифт
    QFont font("Monospace", optimalFontSize, QFont::Bold);
    font.setKerning(false); // Защита от наползания букв при масштабировании
    painter.setFont(font);

    // Рисуем темную подложку (Alpha = 180) СТРОГО внутри защищенного safeRect
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 180)); 
    painter.drawRect(safeRect);

    // Восстанавливаем режим наложения для плавного текста
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);

    // Меняем цвет текста на зеленый (Alpha = 255)
    painter.setPen(Qt::green);
    
    // =========================================================================
    // ВЫЧИСЛЕНИЕ ПРОШЕДШЕГО ВРЕМЕНИ ТАЙМЕРА (БЕЗ МИЛЛИСЕКУНД)
    // =========================================================================
    // Статический таймер, который запускает отсчет ровно ОДИН раз при старте программы
    static QElapsedTimer timerTimer;
    static bool isTimerStarted = false;
    if (!isTimerStarted) {
        timerTimer.start();
        isTimerStarted = true;
    }

    qint64 totalSeconds = timerTimer.elapsed() / 1000; // Переводим миллисекунды в секунды

    // Раскладываем секунды на часы, минуты и секунды таймера
    int hours   = static_cast<int>(totalSeconds / 3600);
    int minutes = static_cast<int>((totalSeconds % 3600) / 60);
    int seconds = static_cast<int>(totalSeconds % 60);

    // Формируем жесткую монолитную строку из 8 символов вида "00:00:00"
    // Используем .arg с заполнением нулями '0' и шириной поля 2
    QString timeStr = QString("%1:%2:%3")
                      .arg(hours,   2, 10, QChar('0'))
                      .arg(minutes, 2, 10, QChar('0'))
                      .arg(seconds, 2, 10, QChar('0'));
    // =========================================================================
    
    // Рисуем текст таймера строго внутри safeRect с центрированием
    painter.drawText(safeRect, Qt::AlignCenter, timeStr);
    
    painter.end();
    return img;
}



QImage OverlayWorker::generateTimeImageCyan(int clock_width, int clock_height) {
   // 1. Maintain Format_RGBA8888 for native 4-byte indexing inside the FPGA worker
    QImage img(clock_width, clock_height, QImage::Format_RGBA8888);
    
    // 2. Initialize with absolute transparent color (Alpha = 0)
    img.fill(Qt::transparent); 

    // =================================================================
    // АДАПТИВНЫЙ РАСЧЕТ РАЗМЕРА ШРИФТА (ДЛЯ СТРОКИ ИЗ 10 СИМВОЛОВ)
    // =================================================================
    // Строка времени вида "hh:mm:ss.z" содержит ровно 10 символов.
    // Задаем расчетную длину 11.2 символа для мягких отступов по бокам.
    // У моноширинного шрифта (Monospace) пропорция ширины к высоте равна 0.6.
    int fontSizeByWidth = static_cast<int>(clock_width / (11.2 * 0.6));
    int fontSizeByHeight = static_cast<int>(clock_height * 0.82); // 18% запас по вертикали
    
    // Выбираем минимальный размер, чтобы текст гарантированно влез в рамки ячейки
    int optimalFontSize = (fontSizeByWidth < fontSizeByHeight) ? fontSizeByWidth : fontSizeByHeight;
    if (optimalFontSize < 8) optimalFontSize = 8; // Защита от нулевого шрифта
    // =================================================================

    QPainter painter(&img);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    // Конфигурируем адаптивный моноширинный шрифт
    QFont font("Monospace", optimalFontSize, QFont::Bold);
    font.setKerning(false); // Защита от наползания букв при масштабировании
    painter.setFont(font);

    // Рисуем подложку (Alpha = 180)
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 180)); 
    painter.drawRect(img.rect());

    // Восстанавливаем режим наложения для плавного текста
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);

    // Меняем цвет текста на зеленый (Alpha = 255)
    painter.setPen(Qt::cyan);
    
    // БЕЗОПАСНОЕ ФИКСИРОВАННОЕ ФОРМАТИРОВАНИЕ В 10 СИМВОЛОВ:
    // 1. Сначала получаем гарантированную строку из 12 символов: "hh:mm:ss.zzz" (например, "15:30:45.000")
    QString fullTimeStr = QTime::currentTime().toString("hh:mm:ss.zzz");
    
    // 2. Жестко отрезаем ровно 10 первых символов слева: "hh:mm:ss.z"
    // Теперь даже если там ноль (например, ".0"), он физически останется в строке!
    QString timeStr = fullTimeStr.left(10);
    
    // Теперь строка ВСЕГДА имеет длину 10 символов, центр не смещается, 
    // и часы больше никогда не будут прыгать и дергаться на экране!
    painter.drawText(img.rect(), Qt::AlignCenter, timeStr);
    
    painter.end();
    return img;
}

QImage OverlayWorker::generateTimeImageMagentaAlpha(int clock_width, int clock_height) {
    QImage img(clock_width, clock_height, QImage::Format_RGBA8888);
    img.fill(Qt::transparent); // Вся картинка изначально полностью прозрачна (Alpha = 0)

    // =========================================================================
    // ЖЕСТКИЙ ЗАЩИТНЫЙ МАКЕТ С ОТСТУПОМ ОТ СЕТКИ СЛЕВА И СПРАВА
    // =========================================================================
    // Мы умышленно сдвигаем начало на 5 пикселей вправо и уменьшаем ширину на 10 пикселей!
    // Теперь края холста часов (шириной по 5 пикселей) гарантированно останутся с Alpha = 0!
    QRect safeRect(5, 0, clock_width - 10, clock_height);
    // =========================================================================

    // Адаптивный расчет шрифта (оставляем твою отличную рабочую математику)
    int fontSizeByWidth = static_cast<int>(clock_width / (9.2 * 0.6));
    int fontSizeByHeight = static_cast<int>(clock_height * 0.82); 
    int optimalFontSize = (fontSizeByWidth < fontSizeByHeight) ? fontSizeByWidth : fontSizeByHeight;
    if (optimalFontSize < 8) optimalFontSize = 8;

    QPainter painter(&img);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    QFont font("Monospace", optimalFontSize / 1.5, QFont::Bold);
    font.setKerning(false); 
    painter.setFont(font);

    // 1. Рисуем подложку ЧЕТКО внутри защищенного прямоугольника safeRect
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 180)); 
    painter.drawRect(safeRect); // Рисуем только в безопасной зоне!

    // 2. Рисуем текст ЧЕТКО внутри защищенного прямоугольника safeRect
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    painter.setPen(Qt::red); // Твоя красивая красная перо-надпись
       
    QString timeStr = QTime::currentTime().toString("hh:mm:ss");
    
    // FIXED: Передаем safeRect вместо img.rect()! 
    // Qt выполнит центрирование строго внутри безопасной зоны, не дотягиваясь до краев ячейки!
    painter.drawText(safeRect, Qt::AlignCenter, timeStr);
    
    painter.end();
    return img;
}


// Функция генерации динамических часов высокого контраста YellowClock
QImage OverlayWorker::generateYellowClock(int clock_width, int clock_height) {
    // optimalFontSize: Математический алгоритм берет ширину clock_width и высоту clock_height. 
    // Он вычисляет, какой размер букв будет идеален, чтобы строчка hh:mm:ss.zzz (12 символов) заполнила ячейку 
    // максимально красиво, не вылезая за рамки [4.4].Сброс кэша по размеру (old_w != canvas_w): 
    // Если вы на лету в файле control.txt поменяете плотность сетки, функция сразу увидит изменение 
    // геометрии холста, сотрет старый кэш и перерисует базовые часы hh:mm:ss 
    // font.setKerning(false): Этот метод принудительно запрещает шрифтовому движку Qt сближать символы [4.4]. 
    // Точка и цифра теперь имеют строго зафиксированные границы и физически не могут наехать друг на друга.
    // Пиксельный расчет base_time_width: Мы математически вычислили длину строки "hh:mm:ss" в пикселях на основе 
    // моноширинного шага [4.4].Qt::AlignLeft для миллисекунд: Новое окно миллисекунд начинается ровно стык в стык там, 
    // где кончаются секунды. Мы печатаем строку ".zzz" с выравниванием влево [4.4]. Точка ложится в память идеально ровно, 
    // сохраняя одинаковое расстояние как до секунд, так и до первой цифры миллисекунд. Строка выглядит монолитной и 
    // больше не слипается.
    const int canvas_w = clock_width; 
    const int canvas_h = clock_height; 

    // =================================================================
    // АДАПТИВНЫЙ РАСЧЕТ РАЗМЕРА ШРИФТА (С ЗАПАСОМ НА ПРАВЫЙ КРАЙ)
    // =================================================================
    // FIXED: Считаем, что в строке 13.2 символов вместо 12. 
    // Это создаст автоматический мягкий отступ справа, и последняя цифра больше не срежется!
    int fontSizeByWidth = static_cast<int>(canvas_w / (13.2 * 0.6));
    int fontSizeByHeight = static_cast<int>(canvas_h * 0.82); 
    
    int optimalFontSize = (fontSizeByWidth < fontSizeByHeight) ? fontSizeByWidth : fontSizeByHeight;
    if (optimalFontSize < 8) optimalFontSize = 8;
    // =================================================================

    // Точный расчет ширины символа для моноширинного шрифта (Monospace)
    // В Qt ширина одного символа Bold Monospace составляет примерно 60% от размера шрифта.
    double char_width = optimalFontSize * 0.6;
    
    // Строка "hh:mm:ss" занимает ровно 8 символов. 
    // Считаем точную координату в пикселях, где физически ЗАКАНЧИВАЕТСЯ секундная стрелка
    int base_time_width = static_cast<int>(8 * char_width);

    // Окно под миллисекунды ".zzz" начинается ровно там, где кончились секунды!
    int ms_x_start = base_time_width;
    int ms_box_w = canvas_w - ms_x_start; // Забираем весь оставшийся хвост ячейки

    static QImage staticTimeCache;
    static QString lastSavedSecond = "";
    static bool isCacheInitialized = false;
    static int old_w = 0;
    static int old_h = 0;

    QTime currentTime = QTime::currentTime();
    QString currentSecondStr = currentTime.toString("hh:mm:ss");

    if (!isCacheInitialized || currentSecondStr != lastSavedSecond || old_w != canvas_w || old_h != canvas_h) {
        staticTimeCache = QImage(canvas_w, canvas_h, QImage::Format_RGBA8888);
        staticTimeCache.fill(Qt::transparent);

        QPainter cachePainter(&staticTimeCache);
        cachePainter.setRenderHint(QPainter::TextAntialiasing, true);
        cachePainter.setCompositionMode(QPainter::CompositionMode_Source);

        QFont font("Monospace", optimalFontSize, QFont::Bold);
        // КРИТИЧЕСКИЙ ФЛАГ: Отключаем кернинг и форсируем жесткое посимвольное расстояние,
        // чтобы буквы и точки никогда не слипались при масштабировании!
        font.setKerning(false);
        cachePainter.setFont(font);

        cachePainter.setPen(Qt::NoPen);
        cachePainter.setBrush(QColor(0, 0, 0, 4));
        cachePainter.drawRect(staticTimeCache.rect());

        cachePainter.setCompositionMode(QPainter::CompositionMode_SourceOver);
        cachePainter.setPen(Qt::yellow);
        
        // Рисуем базовое время строго от левого края (X=0)
        cachePainter.drawText(staticTimeCache.rect(), Qt::AlignLeft | Qt::AlignVCenter, currentSecondStr);
        cachePainter.end();

        lastSavedSecond = currentSecondStr;
        old_w = canvas_w;
        old_h = canvas_h;
        isCacheInitialized = true;
    }

    // Создаем независимый локальный холст кадра
    QImage img(canvas_w, canvas_h, QImage::Format_RGBA8888);
    memcpy(img.bits(), staticTimeCache.constBits(), staticTimeCache.sizeInBytes());

    QPainter painter(&img);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    
    // Вырезаем прямоугольник миллисекунд строго по рассчитанной границе секунд
    QRect ms_rect(ms_x_start, 0, ms_box_w, canvas_h);
    
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 4));
    painter.drawRect(ms_rect);

    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    
    QFont font("Monospace", optimalFontSize / 1.5, QFont::Bold);
    font.setKerning(false); // Защита от склеивания глифов
    painter.setFont(font);
    painter.setPen(Qt::yellow);

    QString msStr = currentTime.toString(".zzz");
    
    // FIXED: Вместо AlignRight используем AlignLeft! 
    // Точка нарисуется строго вплотную к последней цифре секунд с правильным межсимвольным шагом.
    painter.drawText(ms_rect, Qt::AlignLeft | Qt::AlignVCenter, msStr);
    painter.end();

    return img;
}

// сетка
QImage OverlayWorker::generateFullScreenGrid(int horizontalLinesCount, int verticalLinesCount) {
    // Холст на весь экран, но он статичный, поэтому шину не перегрузит!
    QImage img(1920, 1080, QImage::Format_RGBA8888);
    img.fill(Qt::black); // Черный цвет сотрется конвертером воркера в прозрачный YUV [4.4]

    QPainter painter(&img);
    painter.setRenderHint(QPainter::Antialiasing, false); // Четкие линии без размытия

    // Настраиваем серое перо толщиной 2 пикселя
    QPen pen(Qt::gray, 2, Qt::SolidLine, Qt::SquareCap);
    painter.setPen(pen);

    // 1. Рисуем горизонтальные линии на всю ширину (1920)
    if (horizontalLinesCount > 0) {
        double stepY = 1080.0 / (horizontalLinesCount + 1);
        for (int i = 1; i <= horizontalLinesCount; ++i) {
            int y = static_cast<int>(i * stepY);
            painter.drawLine(0, y, 1920, y);
        }
    }

    // 2. Рисуем вертикальные линии на всю высоту (1080)
    if (verticalLinesCount > 0) {
        double stepX = 1920.0 / (verticalLinesCount + 1);
        for (int i = 1; i <= verticalLinesCount; ++i) {
            int x = static_cast<int>(i * stepX);
            painter.drawLine(x, 0, x, 1080);
        }
    }

    painter.end();
    return img;
}

// Label
QImage OverlayWorker::generateLabel(int label_width, int label_height, int labelNumber, QString labelTitle) {
    QImage img(label_width, label_height, QImage::Format_RGBA8888);
    img.fill(Qt::transparent); 

    QString fullText = QString("%1. %2").arg(labelNumber).arg(labelTitle);

    // =================================================================
    // УМНЫЙ АДАПТИВНЫЙ РАСЧЕТ ШРИФТА ПО САМОМУ ДЛИННОМУ СЛОВУ
    // =================================================================
    // 1. Разбиваем текст на отдельные слова, чтобы найти самое длинное
    QStringList words = fullText.split(' ');
    int maxWordLength = 0;
    for (const QString &word : words) {
        if (word.length() > maxWordLength) {
            maxWordLength = word.length(); // Найдет "Temperature" (11 символов)
        }
    }
    // Страховка от слишком маленьких значений
    if (maxWordLength < 4) maxWordLength = 4;

    // Резервируем внутренние отступы плашки (по 15 пикселей с каждого борта ячейки)
    int usable_width = label_width - 30;
    int usable_height = label_height - 15;
    if (usable_width < 10) usable_width = 10;
    if (usable_height < 10) usable_height = 10;

    // 2. Считаем размер шрифта так, чтобы самое длинное слово гарантированно влезло в ОДНУ строку
    int fontSizeByWidth = static_cast<int>(usable_width / (maxWordLength * 0.6));
    
    // 3. Считаем размер шрифта по высоте, учитывая, что у нас будет строго 2 строки текста
    int fontSizeByHeight = static_cast<int>(usable_height / 2.3); // 2.3 учитывает межстрочный интервал
    
    // 4. Выбираем минимальный — самый безопасный размер
    int optimalFontSize = (fontSizeByWidth < fontSizeByHeight) ? fontSizeByWidth : fontSizeByHeight;
    
    // Защита от дурака: ограничиваем максимальный размер, чтобы на огромных ячейках текст не был гигантским
    if (optimalFontSize > 48) optimalFontSize = 48; 
    if (optimalFontSize < 8) optimalFontSize = 8;
    // =================================================================

    QPainter painter(&img);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    painter.setRenderHint(QPainter::Antialiasing, true);

    // Рисуем подложку плашки
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(20, 20, 20, 180));
    
    QRect plateRect(4, 4, label_width - 8, label_height - 8);
    painter.drawRoundedRect(plateRect, 8, 8); 

    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    
    // ПРИМЕНЯЕМ ЧИСТЫЙ optimalFontSize (больше никаких ручных делений / 2 не нужно!)
    QFont font("Arial", optimalFontSize, QFont::Bold);
    font.setKerning(false); // Отключаем микро-сдвиги для четкости на ПЛИС
    painter.setFont(font);
    painter.setPen(Qt::white); 

    // Многострочный перенос с идеальным выравниванием
    painter.drawText(plateRect, Qt::AlignCenter | Qt::TextWordWrap, fullText);
    painter.end();

    return img;
}

// Input static
QImage OverlayWorker::generateInputStatic(int inputStatic_width, int inputStatic_height, int inputStaticNumber, QString inputStaticTitle){
    QImage img(inputStatic_width, inputStatic_height, QImage::Format_RGBA8888);
    img.fill(Qt::transparent); 

    QString fullText = QString("%1. %2").arg(inputStaticNumber).arg(inputStaticTitle);

    // =================================================================
    // УМНЫЙ АДАПТИВНЫЙ РАСЧЕТ ШРИФТА ПО САМОМУ ДЛИННОМУ СЛОВУ
    // =================================================================
    // 1. Разбиваем текст на отдельные слова, чтобы найти самое длинное
    QStringList words = fullText.split(' ');
    int maxWordLength = 0;
    for (const QString &word : words) {
        if (word.length() > maxWordLength) {
            maxWordLength = word.length(); // Найдет "Temperature" (11 символов)
        }
    }
    // Страховка от слишком маленьких значений
    if (maxWordLength < 4) maxWordLength = 4;

    // Резервируем внутренние отступы плашки (по 15 пикселей с каждого борта ячейки)
    int usable_width = inputStatic_width - 30;
    int usable_height = inputStatic_height - 15;
    if (usable_width < 10) usable_width = 10;
    if (usable_height < 10) usable_height = 10;

    // 2. Считаем размер шрифта так, чтобы самое длинное слово гарантированно влезло в ОДНУ строку
    int fontSizeByWidth = static_cast<int>(usable_width / (maxWordLength * 0.6));
    
    // 3. Считаем размер шрифта по высоте, учитывая, что у нас будет строго 2 строки текста
    int fontSizeByHeight = static_cast<int>(usable_height / 2.3); // 2.3 учитывает межстрочный интервал
    
    // 4. Выбираем минимальный — самый безопасный размер
    int optimalFontSize = (fontSizeByWidth < fontSizeByHeight) ? fontSizeByWidth : fontSizeByHeight;
    
    // Защита от дурака: ограничиваем максимальный размер, чтобы на огромных ячейках текст не был гигантским
    if (optimalFontSize > 48) optimalFontSize = 48; 
    if (optimalFontSize < 8) optimalFontSize = 8;
    // =================================================================

    QPainter painter(&img);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    painter.setRenderHint(QPainter::Antialiasing, true);

    // Рисуем подложку плашки
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(20, 20, 20, 180));
    
    QRect plateRect(4, 4, inputStatic_width - 8, inputStatic_height - 8);
    painter.drawRoundedRect(plateRect, 8, 8); 

    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    
    // ПРИМЕНЯЕМ ЧИСТЫЙ optimalFontSize (больше никаких ручных делений / 2 не нужно!)
    QFont font("Arial", optimalFontSize, QFont::Bold);
    font.setKerning(false); // Отключаем микро-сдвиги для четкости на ПЛИС
    painter.setFont(font);
    painter.setPen(Qt::gray); 

    // Многострочный перенос с идеальным выравниванием
    painter.drawText(plateRect, Qt::AlignCenter | Qt::TextWordWrap, fullText);
    painter.end();

    return img;
}

QImage OverlayWorker::generateYellowClockOnlyMS(int clock_width, int clock_height) {
    // 1. Создаем локальный холст ячейки кадра
    QImage img(clock_width, clock_height, QImage::Format_RGBA8888);
    img.fill(Qt::transparent); // Изначально холст полностью прозрачный

    // =================================================================
    // АДАПТИВНЫЙ РАСЧЕТ РАЗМЕРА ШРИФТА (ДЛЯ СТРОКИ ИЗ 10 СИМВОЛОВ hh:mm:ss.z)
    // =================================================================
    // Строка времени вида "hh:mm:ss.z" содержит ровно 10 символов.
    // Задаем расчетную длину 11.2 символа, чтобы сделать мягкие отступы слева и справа.
    // У моноширинного шрифта (Monospace) пропорция ширины к высоте равна 0.6.
    int fontSizeByWidth = static_cast<int>(clock_width / (11.2 * 0.6));
    int fontSizeByHeight = static_cast<int>(clock_height * 0.82); // 18% запас по вертикали
    
    // Выбираем минимальный размер, чтобы текст гарантированно влез в рамки ячейки
    int optimalFontSize = (fontSizeByWidth < fontSizeByHeight) ? fontSizeByWidth : fontSizeByHeight;
    if (optimalFontSize < 8) optimalFontSize = 8; // Защита от нулевого шрифта
    // =================================================================

    QPainter painter(&img);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    // Конфигурируем моноширинный шрифт
    QFont font("Monospace", optimalFontSize, QFont::Bold);
    font.setKerning(false); // Жесткая защита от наползания и склеивания двоеточий, точек и цифр
    painter.setFont(font);
    painter.setPen(Qt::yellow); // Желтый цвет текста

    // =================================================================
    // ФОРМИРОВАНИЕ МОНОЛИТНОЙ СТРОКИ ВРЕМЕНИ С КИЛЛ-ФИЧЕЙ ОТ ПРЫЖКОВ
    // =================================================================
    // 1. Получаем гарантированную строку из 12 символов: "hh:mm:ss.zzz" (например, "12:34:56.000")
    QString fullTimeStr = QTime::currentTime().toString("hh:mm:ss.zzz");
    
    // 2. Жестко отрезаем ровно 10 первых символов слева: "hh:mm:ss.z"
    // Теперь даже если миллисекунды равны нулю (например, ".0"), этот ноль физически
    // останется в строке, длина строки ВСЕГДА будет равна 10 символам, а текст перестанет прыгать!
    QString timeStr = fullTimeStr.left(10);
    // =================================================================

    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);

    // Выводим монолитную строку, идеально центрируя её по всему прямоугольнику ячейки
    painter.drawText(img.rect(), Qt::AlignCenter, timeStr);
    painter.end();

    return img;
}

void OverlayWorker::runStartupGreeting() {
    qDebug() << "STARTUP | Launching pitch-black 4-second greeting loop...";

    QElapsedTimer greetingTimer;
    greetingTimer.start();

    // Предварительно генерируем базовую сетку в ОЗУ
    QImage baseGrid = generateFullScreenGrid(2, 2); 

    while (greetingTimer.elapsed() <= 4000) {
        qint64 elapsed = greetingTimer.elapsed();

        // 1. Нормализуем прогресс времени от 0.0 до 1.0
        double progress = elapsed / 4000.0;

        // --- МАТЕМАТИКА ТЕКСТА (Синусоида) ---
        // Начнется в 0, плавно взлетит до 1.0 на 2-й секунде, и уйдет в 0 к концу кадра
        double textSmoothFactor = std::sin(progress * M_PI);
        int textAlpha = static_cast<int>(textSmoothFactor * 255.0);
        if (textAlpha < 0) textAlpha = 0;
        if (textAlpha > 255) textAlpha = 255;

        // --- МАТЕМАТИКА ФОНА (Линейное затухание от 255 до 0) ---
        // В момент старта progress = 0.0 -> bgAlpha = 255 (Абсолютная темнота).
        // К концу progress = 1.0 -> bgAlpha = 0 (Полная прозрачность, открывающая сетку).
        int bgAlpha = static_cast<int>((1.0 - progress) * 255.0);
        if (bgAlpha < 0) bgAlpha = 0;
        if (bgAlpha > 255) bgAlpha = 255;

        // --- СБОРКА КАДРА В ОЗУ ПРОЦЕССОРА ---
        QImage greetingCanvas(1920, 1080, QImage::Format_RGBA8888);
        greetingCanvas.fill(Qt::transparent);

        QPainter canvasPainter(&greetingCanvas);
        canvasPainter.setRenderHint(QPainter::Antialiasing, true);
        canvasPainter.setRenderHint(QPainter::TextAntialiasing, true);

        // Слой 1: Рисуем базовую сетку из кэша ОЗУ
        if (!baseGrid.isNull()) {
            canvasPainter.drawImage(0, 0, baseGrid);
        }

        // Слой 2: Накладываем маску КРИСТАЛЬНОЙ ТЕМНОТЫ
        // На старте она намертво перекроет сетку, исключая любые вспышки интерфейса!
        canvasPainter.setPen(Qt::NoPen);
        canvasPainter.setBrush(QColor(0, 0, 0, bgAlpha));
        canvasPainter.drawRect(greetingCanvas.rect());

        // Слой 3: Рисуем плывущий оранжевый текст
        int red = static_cast<int>(textSmoothFactor * 255.0);
        int green = static_cast<int>(textSmoothFactor * 120.0);
        QColor dynamicOrange(red, green, 0, textAlpha);

        int bigFontSize = 1920 / 14; 
        QFont font("Arial", bigFontSize, QFont::Bold);
        font.setKerning(false);
        canvasPainter.setFont(font);
        canvasPainter.setPen(dynamicOrange);

        canvasPainter.drawText(greetingCanvas.rect(), Qt::AlignCenter, "Profitt PBX-MTV-5161");
        canvasPainter.end();

        // --- ОТПРАВКА В DDR ПЛИС ЧЕРЕЗ НАШ БЫСТРЫЙ НЕОН ---
        this->convert_and_write_rect(greetingCanvas, 0, 0);

        // Переключаем буферы
        this->flip_buffer();

        // Пауза 15 мс (~60 кадров в секунду для плавной анимации)
        QThread::msleep(15); 
    }

    qDebug() << "STARTUP | Cinematic greeting completed.";
}

void OverlayWorker::process_and_write_full_scene(const QList<SceneElementData> &sceneData) {
    m_is_busy = true; // Блокируем очередь

    QElapsedTimer workerTimer;
    workerTimer.start();

    // Переменные для отслеживания хода времени и смены конфигурации
    static QString lastSecond = "";
    static int last_grid_w = -1;
    static int last_grid_h = -1;

    // Счётчики для размазывания ежесекундного обновления на ОБА буфера ПЛИС
    static int static_update_buffer_countdown = 0;
    static int second_update_buffer_countdown = 0;

    // Статический кэш картинок, чтобы не рендерить их QPainter-ом каждый кадр
    static QImage cachedGrid;
    static QMap<int, QImage> cachedLabels;

    QTime currentTime = QTime::currentTime();
    QString currentSecondStr = currentTime.toString("hh:mm:ss");

    // Проверяем, изменились ли параметры сетки в файле
    bool gridChanged = false;
    for (const SceneElementData& element : sceneData) {
        if (element.typeId == 0) {
            if (element.width != last_grid_w || element.height != last_grid_h) {
                gridChanged = true;
                last_grid_w = element.width;
                last_grid_h = element.height;
            }
            break;
        }
    }

    // Если сетка изменилась (или это самый первый старт), взводим счётчик на 2 кадра!
    // Мы заставим воркер впекать статику два кадра подряд, чтобы заполнить ОБА буфера ПЛИС!
    if (gridChanged) {
        cachedGrid = QImage();
        cachedLabels.clear();
        lastSecond = ""; // Сбрасываем секунду для автообновления часов
        static_update_buffer_countdown = 2; // Жестко: пишем статику в следующие 2 кадра
    }

    // Если сменилась секунда, взводим счётчик на 2 кадра для обновления hh:mm:ss в ОБОИХ буферах!
    if (currentSecondStr != lastSecond) {
        second_update_buffer_countdown = 2;
        lastSecond = currentSecondStr;
    }

    // ВЫЧИСЛЯЕМ АДРЕС ТЕКУЩЕГО СКРЫТОГО БУФЕРА ЗАПИСИ
    int nextWriteIndex = (m_current_display_idx == 0) ? 1 : 0;
    unsigned char* active_fb_ptr = m_mmap_base + (nextWriteIndex * FRAME_SIZE);

    // =========================================================================
    // ЭТАП А1: ЗАПИСЬ ТЯЖЕЛОЙ СТАТИКИ (Отрабатывает 2 кадра подряд ТОЛЬКО при старте/смене сетки)
    // =========================================================================
    if (static_update_buffer_countdown > 0) {
        // 1. Генерируем FullHD холст сетки (typeId 0), если он пуст
        for (const SceneElementData& element : sceneData) {
            if (element.typeId == 0 && cachedGrid.isNull()) {
                cachedGrid = generateFullScreenGrid(element.width, element.height);
                break;
            }
        }

        // 2. Впекаем СЕТКУ как базовый задний план на весь текущий скрытый буфер DDR ПЛИС
        if (!cachedGrid.isNull()) {
            this->convert_and_write_rect(cachedGrid, 0, 0); 
            qDebug() << "BURN | Grid successfully written into hardware buffer index:" << nextWriteIndex;
        }

        // 3. Генерируем и впекаем в текущий буфер все СТАТИЧЕСКИЕ ЯРЛЫКИ (typeId 6)
        for (const SceneElementData& element : sceneData) {
            if (element.typeId == 6) {
                if (!cachedLabels.contains(element.labelNum)) {
                    QImage newLabel = generateLabel(element.width, element.height, element.labelNum, element.labelTitle);
                    cachedLabels.insert(element.labelNum, newLabel);
                }
                QImage lblImg = cachedLabels.value(element.labelNum);
                this->convert_and_write_rect(lblImg, element.offset_x, element.offset_y);
            }
        }

        static_update_buffer_countdown--; // Уменьшаем счётчик кадров для статики
    }

    // =========================================================================
    // ЭТАП А2: СМЕНА СЕКУНДЫ (Отрабатывает 2 кадра подряд раз в секунду для ОБОИХ буферов)
    // =========================================================================
    if (second_update_buffer_countdown > 0) {
        for (const SceneElementData& element : sceneData) {
            if (element.typeId == 3 || element.typeId == 4) {
                
                // Перед тем как нарисовать новые цифры секунд, затираем старые куском ЧИСТОЙ сетки из кэша!
                if (!cachedGrid.isNull()) {
                    QImage cleanField = cachedGrid.copy(element.offset_x, element.offset_y, element.width, element.height);
                    this->convert_and_write_rect(cleanField, element.offset_x, element.offset_y);
                }

                // =================================================================
                // FIXED ДЛЯ ЗАЩИТЫ ВЕРТИКАЛЬНОЙ СЕТКИ: ИЗОЛИРУЕМ ШИРИНУ ХОЛСТА ЧАСОВ
                // =================================================================
                // Уменьшаем ширину холста часов на 8 пикселей (размер одного векторного шага NEON).
                // Сдвигаем координату X на 4 пикселя вправо, чтобы часы встали ровно по центру ячейки.
                int safe_clock_width = element.width - 8;
                int safe_clock_x = element.offset_x + 4;
                // =================================================================

                // Генерируем и впекаем новые цифры времени hh:mm:ss, используя безопасные размеры
                if (element.typeId == 3) {
                    QImage img = generateTimeImageGreen(safe_clock_width, element.height);
                    this->convert_and_write_rect(img, safe_clock_x, element.offset_y);
                } else {
                    QImage img = generateTimeImageMagentaAlpha(safe_clock_width, element.height);
                    this->convert_and_write_rect(img, safe_clock_x, element.offset_y);
                }
            }
        }
        second_update_buffer_countdown--; 
    }

        // =========================================================================
    // ЭТАП Б: ОБНОВЛЕНИЕ СВЕРХДИНАМИКИ (Выполняется КАЖДЫЙ КАДР таймера)
    // =========================================================================
    
    // 1. Желтые часы (typeId == 1) — Полная строка времени hh:mm:ss.z на каждом кадре!
    for (const SceneElementData& element : sceneData) {
        if (element.typeId == 1) {
            // Очищаем старую зону ячейки целиком куском чистой сетки из кэша
            if (!cachedGrid.isNull()) {
                QImage cleanField = cachedGrid.copy(element.offset_x, element.offset_y, element.width, element.height);
                this->convert_and_write_rect(cleanField, element.offset_x, element.offset_y);
            }

            // Генерируем полную желтую строку времени и впекаем в текущий буфер DDR ПЛИС
            QImage yellowImg = generateYellowClockOnlyMS(element.width, element.height); 
            this->convert_and_write_rect(yellowImg, element.offset_x, element.offset_y); 
        }
    }

    // 2. Аналоговые часы (typeId == 2) — стрелки кадра
    for (const SceneElementData& element : sceneData) {
        if (element.typeId == 2) {
            // SAFE MARGIN ДЛЯ СТРЕЛОК:
            // Делаем отступ от серых границ ячейки по 2 пикселя со всех сторон.
            // Теперь затирочный memcpy/NEON физически не дотянется до вертикальных линий сетки!
            int safe_w = element.width - 4;
            int safe_h = element.height - 4;

            if (!cachedGrid.isNull() && safe_w > 0 && safe_h > 0) {
                // Копируем чистую сетку с безопасным сдвигом (+2, +2)
                QImage cleanField = cachedGrid.copy(element.offset_x + 2, element.offset_y + 2, safe_w, safe_h);
                this->convert_and_write_rect(cleanField, element.offset_x + 2, element.offset_y + 2);
            }

            // Впекаем новые стрелки часов прямо поверх сетки в DDR текущего буфера
            QImage analogImg = renderSvgToImage_no_parse_arrows(element.height * 2, element.width * 2);
            this->convert_and_write_rect(analogImg, element.offset_x, element.offset_y);
        }
    }

    qint64 totalWorkerTimeMs = workerTimer.elapsed();

    // Логирование статистики воркера
    static int workerLogCounter = 0;
    workerLogCounter++;
    if (workerLogCounter % 50 == 0) {
        qDebug().noquote() << "================== ULTRA-FAST DIRECT DDR WORKER ==================";
        qDebug().noquote() << QString("Worker Intel | Total Frame Process Time : %1 ms (Direct DDR)")
                              .arg(totalWorkerTimeMs);
        qDebug().noquote() << "==================================================================";
    }

    // Физически переключаем готовый буфер памяти в ПЛИС
    this->flip_buffer(); 
    m_is_busy = false; // Освобождаем воркер
}


// QImage OverlayWorker::generateTimeImageMagentaAlpha(int clock_width, int clock_height) {
//     // Чтобы текст идеально вписывался в холст clock_width на clock_height и не обрезался по краям из-за боковых 
//     // полей шрифта Linux, мы заложим безопасную длину строки в 9.2 символа вместо 8. 
//     // Также мы применим метод font.setKerning(false), чтобы гарантировать отсутствие микро-сдвигов букв на ПЛИС.

//     // 1. Maintain Format_RGBA8888 for native 4-byte indexing inside the FPGA worker
//     QImage img(clock_width, clock_height, QImage::Format_RGBA8888);
    
//     // 2. Initialize with absolute transparent color (Alpha = 0)
//     img.fill(Qt::transparent);

//     // =================================================================
//     // АДАПТИВНЫЙ РАСЧЕТ РАЗМЕРА ШРИФТА (FONT SCALING)
//     // =================================================================
//     // Строка "hh:mm:ss" имеет ровно 8 символов. 
//     // Задаем расчетную длину 9.2 символа, чтобы сделать мягкие отступы слева и справа.
//     // У моноширинного шрифта (Monospace) пропорция ширины к высоте равна 0.6.
//     int fontSizeByWidth = static_cast<int>(clock_width / (9.2 * 0.6));
//     int fontSizeByHeight = static_cast<int>(clock_height * 0.82); // 18% запас на верхний/нижний отступы
    
//     // Выбираем минимальный размер, чтобы текст гарантированно влез в рамки ячейки
//     int optimalFontSize = (fontSizeByWidth < fontSizeByHeight) ? fontSizeByWidth : fontSizeByHeight;
//     if (optimalFontSize < 8) optimalFontSize = 8; // Никакого нулевого шрифта
//     // =================================================================

//     QPainter painter(&img);
//     painter.setRenderHint(QPainter::TextAntialiasing, true);

//     // Конфигурируем адаптивный моноширинный шрифт
//     QFont font("Monospace", optimalFontSize / 1.5, QFont::Bold);
//     font.setKerning(false); // Защита от наползания букв при масштабировании
//     painter.setFont(font);

//     // 4. Draw background plate with strict Alpha = 180 (или 4, смотря как настроена маска ПЛИС)
//     painter.setPen(Qt::NoPen);
//     painter.setBrush(QColor(0, 0, 0, 180)); 
//     // БЫЛО: painter.drawRect(img.rect());
    
//     // СТАНЕТ (ИСПРАВЛЕНО ДЛЯ КРАСНЫХ И ЗЕЛЕНЫХ ЧАСОВ):
//     // Делаем мягкий отступ подложки на 2 пикселя слева и справа.
//     // Теперь края холста будут иметь Alpha = 0, и конвертер никогда не затрет серую сетку!
//     QRect safeBackgroundRect(2, 0, clock_width - 4, clock_height);
    
//     painter.setPen(Qt::NoPen);
//     painter.setBrush(QColor(0, 0, 0, 180)); 
//     painter.drawRect(safeBackgroundRect);

//     // 5. Restore composition mode to regular blending for the text,
//     // so anti-aliased font edges draw smoothly on top of our alpha=180 canvas.
//     painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    
//     // Задаем цвет (благодаря нашему YUV CLAMPING 16-235 в воркере, Qt::magenta теперь безопасен!)
//     painter.setPen(Qt::red); 
       
//     // Рисуем время "hh:mm:ss", центрируя его идеально по всему прямоугольнику ячейки
//     QString timeStr = QTime::currentTime().toString("hh:mm:ss");
//     painter.drawText(img.rect(), Qt::AlignCenter, timeStr);
    
//     painter.end();
//     return img;
// }



// QImage OverlayWorker::generateYellowClockOnlyMS(int clock_width, int clock_height) {
//     // variant zzz
//     QImage img(clock_width, clock_height, QImage::Format_RGBA8888);
//     img.fill(Qt::transparent); // Левая часть (где базовое время) будет прозрачной!

//     int fontSizeByWidth = static_cast<int>(clock_width / (13.2 * 0.6));
//     int fontSizeByHeight = static_cast<int>(clock_height * 0.82); 
//     int optimalFontSize = (fontSizeByWidth < fontSizeByHeight) ? fontSizeByWidth : fontSizeByHeight;

//     double char_width = optimalFontSize * 0.6;
//     int ms_x_start = static_cast<int>(8 * char_width);
//     int ms_box_w = clock_width - ms_x_start;

//     QPainter painter(&img);
//     painter.setRenderHint(QPainter::TextAntialiasing, true);
    
//     // Вырезаем прямоугольник строго под миллисекунды
//     QRect ms_rect(ms_x_start, 0, ms_box_w, clock_height);

//     painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
//     QFont font("Monospace", optimalFontSize, QFont::Bold);
//     font.setKerning(false);
//     painter.setFont(font);
//     painter.setPen(Qt::yellow);

//     QString msStr = QTime::currentTime().toString(".z");
//     painter.drawText(ms_rect, Qt::AlignLeft | Qt::AlignVCenter, msStr);
//     painter.end();

//     return img;
// }
