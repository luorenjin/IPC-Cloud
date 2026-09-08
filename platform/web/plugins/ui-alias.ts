// 组件别名：把 components/ui/* 同时以无后缀名（Icon/Card/Table…）全局注册，
// 避免模板中 <Icon> 解析为未知自定义元素。
import Icon from '~/components/ui/Icon.vue'
import Button from '~/components/ui/Button.vue'
import Input from '~/components/ui/Input.vue'
import Select from '~/components/ui/Select.vue'
import Table from '~/components/ui/Table.vue'
import Pagination from '~/components/ui/Pagination.vue'
import Dialog from '~/components/ui/Dialog.vue'
import Drawer from '~/components/ui/Drawer.vue'
import Tabs from '~/components/ui/Tabs.vue'
import Segmented from '~/components/ui/Segmented.vue'
import Switch from '~/components/ui/Switch.vue'
import Checkbox from '~/components/ui/Checkbox.vue'
import RadioGroup from '~/components/ui/RadioGroup.vue'
import Slider from '~/components/ui/Slider.vue'
import Tag from '~/components/ui/Tag.vue'
import Badge from '~/components/ui/Badge.vue'
import Card from '~/components/ui/Card.vue'
import EmptyState from '~/components/ui/EmptyState.vue'
import Loading from '~/components/ui/Loading.vue'
import Dropdown from '~/components/ui/Dropdown.vue'
import Popover from '~/components/ui/Popover.vue'
import Tooltip from '~/components/ui/Tooltip.vue'
import Steps from '~/components/ui/Steps.vue'
import Tree from '~/components/ui/Tree.vue'
import ErrorCard from '~/components/ui/ErrorCard.vue'

export default defineNuxtPlugin((nuxtApp) => {
  const map: Record<string, any> = {
    Icon, Button, Input, Select, Table, Pagination, Dialog, Drawer, Tabs, Segmented,
    Switch, Checkbox, RadioGroup, Slider, Tag, Badge, Card, EmptyState, Loading,
    Dropdown, Popover, Tooltip, Steps, Tree, ErrorCard
  }
  for (const [name, comp] of Object.entries(map)) {
    nuxtApp.vueApp.component(name, comp)
  }
})
