#include "placeholder_widget.h"

PlaceholderWidget::PlaceholderWidget(lv_obj_t* parent, const char* text, int column_span,
                                      int row_span)
    : column_span_(column_span), row_span_(row_span) {
    root_ = lv_obj_create(parent);
    lv_obj_set_style_pad_all(root_, 8, 0);

    lv_obj_t* label = lv_label_create(root_);
    lv_label_set_text(label, text);
    lv_obj_center(label);
}
