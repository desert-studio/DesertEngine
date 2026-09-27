{#- PropertyMetadata of one field. Designated initializers must follow declaration order (DisplayName,
    Category, Tooltip, Header, ...), so the order of these lines is the order of the struct's members. The model
    carries a key only when the property is set. -#}
PropertyMetadata{ {% if f.meta.displayName %}.DisplayName = "{{ f.meta.displayName }}", {% endif -%}
{% if f.meta.category %}.Category = "{{ f.meta.category }}", {% endif -%}
{% if f.meta.tooltip %}.Tooltip = "{{ f.meta.tooltip }}", {% endif -%}
{% if f.meta.header %}.Header = "{{ f.meta.header }}", {% endif -%}
{% if f.meta.hasRange %}.HasRange = true, .RangeMin = {{ f.meta.rangeMin }}, .RangeMax = {{ f.meta.rangeMax }}, {% endif -%}
{% if f.meta.isColor %}.IsColor = true, {% endif -%}
{% if f.meta.isAsset %}.IsAsset = true, .AssetType = "{{ f.meta.assetType }}", {% endif -%}
{% if f.meta.thumbnail %}.Thumbnail = true, {% endif -%}
{% if f.meta.readOnly %}.ReadOnly = true, {% endif -%}
{% if f.meta.hidden %}.Hidden = true, {% endif -%}
{% if f.meta.isLength %}.IsLength = true, {% endif -%}
{% if f.meta.units %}.Units = "{{ f.meta.units }}", {% endif -%}
{% if f.meta.advanced %}.Advanced = true, {% endif -%}
{% if f.meta.summary %}.Summary = true, {% endif -%}
{% if f.meta.temperature %}.Temperature = true, {% endif -%}
{% if f.meta.preview %}.Preview = true, {% endif -%}
{% if f.meta.editCondition %}.EditCondition = "{{ f.meta.editCondition }}", {% endif -%}
}
{#- the include sits mid-line in Field.tpl, so no newline follows the brace -#}
