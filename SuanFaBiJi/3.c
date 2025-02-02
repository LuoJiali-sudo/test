#include <asm/io.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/delay.h>
#include <linux/pid.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/types.h>

// 定义教育设备信息结构体
struct edu_dev_info
{
	resource_size_t io;      // 设备I/O资源大小
    long range, flags;       // 设备范围和标志，这里flags未使用
    void __iomem *ioaddr;    // 设备I/O内存地址
    int irq;                 // 设备中断号
};

// PCI设备ID表，用于匹配本驱动支持的PCI设备
static struct pci_device_id id_table[] = {
    { PCI_DEVICE(0x1234, 0x11e8) }, // 示例设备ID和厂商ID
    { 0, }                          // 列表结束标志
};

// 全局变量，指向edu_dev_info结构体的指针
static struct edu_dev_info *edu_info;
// 自旋锁，用于同步访问edu_info
static spinlock_t lock;

// 设备探测函数
static int edu_driver_probe(struct pci_dev *dev, const struct pci_device_id *id)
{
	//TODO:启用PCI设备
	int ret = 0;
	printk("executing edu driver probe function!\n");
	ret = pci_enable_device(dev);
	if (ret)
	{
		printk(KERN_ERR "IO Error.\n");
		return -EIO;
	}

	//TODO:为 edu_dev_info 实例分配内存 
	edu_info = kmalloc(sizeof(struct edu_dev_info), GFP_KERNEL);
    if (!edu_info) {
        ret = -ENOMEM;
        goto out_mypci;
    }
    edu_info->irq = dev->irq;// 保存设备的中断号
    // 请求PCI资源区域
	ret = pci_request_regions(dev, "edu_dirver"); 
	if (ret)
	{
		printk("PCI request regions err!\n");
		goto out_mypci;
	}

	//TODO: 将 BAR 的总线地址映射到系统内存的虚拟地址 
	edu_info->io = pci_resource_start(dev, 0);
    edu_info->ioaddr = pci_ioremap_bar(dev, 0);
    if (!edu_info->ioaddr) {
        ret = -ENOMEM;
        goto out_regions;
    }
    
 	// 设置PCI设备的私有数据
	pci_set_drvdata(dev, edu_info); 
	printk("Probe succeeds.PCIE ioport addr start at %llX, edu_info->ioaddr is 0x%p.\n", edu_info->io, edu_info->ioaddr);

	return 0;
  // 错误处理路径
out_iounmap:
    iounmap(edu_info->ioaddr); // 取消I/O内存映射
out_regions:
    pci_release_regions(dev);  // 释放PCI资源区域
out_mypci:
    kfree(edu_info);           // 释放edu_dev_info结构体内存
    return ret;
}

// 设备移除函数
static void edu_driver_remove(struct pci_dev *dev)
{
 	edu_info = pci_get_drvdata(dev); // 从PCI设备获取私有数据
    iounmap(edu_info->ioaddr);      // 取消I/O内存映射
    pci_release_regions(dev);       // 释放PCI资源区域
    kfree(edu_info);                // 释放edu_dev_info结构体内存
    pci_disable_device(dev);        // 禁用PCI设备
    printk("Device is removed successfully.\n");
}

// 注册PCI设备ID表
MODULE_DEVICE_TABLE(pci, id_table); 

// 定义PCI驱动结构体
static struct pci_driver pci_driver = {
	.name = "edu_driver",       // 驱动名称
    .id_table = id_table,       // 支持的设备ID表
    .probe = edu_driver_probe,  // 探测函数
    .remove = edu_driver_remove,// 移除函数
};

// =============================================================================== //

#define EDU_DEV_MAJOR 200  // 定义设备的主设备号
#define EDU_DEV_NAME "edu" // 定义设备的名称


int current_id = 0;

struct user_data
{
    int id; // 用户数据的唯一标识符
    atomic64_t data; // 原子类型的数据成员，用于存储计算结果，确保多线程访问时的原子性
};

struct thread_data
{
	struct user_data* user_data_ptr; // 指向用户数据的指针
    unsigned long input_data; // 输入数据，例如阶乘的底数
    struct task_struct* kthread; // 指向内核线程的指针（虽然在这段代码中未使用）
};


int kthread_handler(void *data)
{
	// 转换传入的void*数据为struct thread_data*
	struct thread_data* thread_data_ptr = (struct thread_data*)data;
    unsigned long value = thread_data_ptr->input_data;
	printk("ioctl cmd 0 : factorial\n");
	
	unsigned long result = 1;
	unsigned long i = 1;
    
    // 计算阶乘
    for ( i = 1; i <= value; i++) {
        result *= i;
    }

    // 使用自旋锁保护对atomic64的写操作，确保原子性
    spin_lock(&lock); // 注意：lock变量需要在某处定义和初始化
    atomic64_set(&thread_data_ptr->user_data_ptr->data, result);
    spin_unlock(&lock);

    // 释放分配的内存
    kfree(thread_data_ptr);
	return 0;
}




static int edu_dev_open(struct inode *inode, struct file *filp)
{
	//TODO:完成 filp 与用户进程上下文信息的绑定操作
	struct user_data* user_data_ptr = kmalloc(sizeof(struct user_data), GFP_KERNEL);
    if (!user_data_ptr)
        return -ENOMEM;

    user_data_ptr->id = current_id++;
    filp->private_data = user_data_ptr;

    return 0;
	
}



static int edu_dev_release(struct inode *inode, struct file *filp)
{
	//TODO:释放 edu_dev_open 中分配的内存 
	struct user_data* user_data_ptr = filp->private_data;
    kfree(user_data_ptr);
    return 0;
}

// edu_dev_unlocked_ioctl 函数用于处理ioctl调用
long edu_dev_unlocked_ioctl(struct file *pfilp_t, unsigned int cmd, unsigned long arg)
{
    // 获取与文件描述符关联的私有数据
    struct user_data* user_data_ptr = pfilp_t->private_data;
    struct thread_data* thread_data;
    unsigned long input_value;

    // 假设只有cmd为0时才处理阶乘计算
    if (cmd != 0) 
        return -EINVAL; // 如果cmd不是0，则返回无效参数错误

    // 从用户空间复制输入值到内核空间
    if (copy_from_user(&input_value, (void __user *)arg, sizeof(input_value)))
        return -EFAULT; // 如果复制失败，则返回坏地址错误

    // 为线程数据分配内存
    thread_data = kmalloc(sizeof(struct thread_data), GFP_KERNEL);
    if (!thread_data)
        return -ENOMEM; // 如果内存分配失败，则返回内存不足错误

    // 初始化线程数据
    thread_data->user_data_ptr = user_data_ptr;
    thread_data->input_data = input_value;

    // 创建一个内核线程来计算阶乘
    thread_data->kthread = kthread_run(kthread_handler, thread_data, "edu_factorial_thread");
    if (IS_ERR(thread_data->kthread)) {
        kfree(thread_data); // 如果线程创建失败，释放已分配的内存
        return PTR_ERR(thread_data->kthread); // 返回错误码
    }

    // 等待线程完成
    kthread_stop(thread_data->kthread);

    return 0; 
}


// 定义文件操作结构
static struct file_operations edu_dev_fops = {
    .open = edu_dev_open,       // 打开设备文件时的回调函数
    .release = edu_dev_release, // 关闭设备文件时的回调函数
    .unlocked_ioctl = edu_dev_unlocked_ioctl, // 处理ioctl调用的回调函数
};

// 模块初始化函数
static int __init edu_dirver_init(void)
{
	printk("HELLO PCI\n");// 打印日志信息
	int ret = 0;
	// 注册字符设备驱动
	ret = register_chrdev(EDU_DEV_MAJOR, EDU_DEV_NAME, &edu_dev_fops);
	if (0 > ret)
	{
		printk("kernel edu dev register_chrdev failure\n");
		return -1;
	}
	printk("chrdev edu dev is insmod, major_dev is 200\n");
	// 注册PCI驱动
	ret = pci_register_driver(&pci_driver);
	if (ret)
	{
		printk("kernel edu dev pci_register_driver failure\n");
		return ret;
	}
	//初始化自旋锁
    spin_lock_init(&lock);
	return 0;
}

// 模块退出函数
static void __exit edu_dirver_exit(void)
{
	// 注销字符设备驱动
	unregister_chrdev(EDU_DEV_MAJOR, EDU_DEV_NAME);
 	// 注销PCI驱动
	pci_unregister_driver(&pci_driver);
	printk("GOODBYE PCI\n"); // 打印日志信息
}

MODULE_LICENSE("GPL");// 指定模块许可证为GPL

module_init(edu_dirver_init);
module_exit(edu_dirver_exit);